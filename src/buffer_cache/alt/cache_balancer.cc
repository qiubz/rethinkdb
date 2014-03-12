#include "buffer_cache/alt/cache_balancer.hpp"

#include "buffer_cache/alt/evicter.hpp"
#include "arch/runtime/runtime.hpp"
#include "concurrency/pmap.hpp"

const uint64_t alt_cache_balancer_t::rebalance_check_interval_ms = 20;
const uint64_t alt_cache_balancer_t::rebalance_access_count_threshold = 100;
const uint64_t alt_cache_balancer_t::rebalance_timeout_ms = 500;

// These variables control the stopping of read-ahead
// Stop read ahead once 90% of the cache has been utilized
const uint64_t alt_cache_balancer_t::read_ahead_ratio_numerator = 9;
const uint64_t alt_cache_balancer_t::read_ahead_ratio_denominator = 10;

alt_cache_balancer_t::cache_data_t::cache_data_t(alt::evicter_t *_evicter) :
    evicter(_evicter),
    new_size(0),
    old_size(evicter->get_memory_limit()),
    bytes_loaded(evicter->get_bytes_loaded()) { }

alt_cache_balancer_t::alt_cache_balancer_t(uint64_t _total_cache_size) :
    total_cache_size(_total_cache_size),
    rebalance_timer(rebalance_check_interval_ms, this),
    last_rebalance_time(0),
    read_ahead_ok(true),
    thread_info(get_num_threads()),
    rebalance_pool(1, &pool_queue, this) { }

alt_cache_balancer_t::~alt_cache_balancer_t() {
    assert_thread();
}

void alt_cache_balancer_t::add_evicter(alt::evicter_t *evicter) {
    evicter->assert_thread();
    thread_info_t *info = &thread_info[get_thread_id().threadnum];
    cross_thread_mutex_t::acq_t mutex_acq(&info->mutex);
    auto res = info->evicters.insert(evicter);
    guarantee(res.second);
}

void alt_cache_balancer_t::remove_evicter(alt::evicter_t *evicter) {
    evicter->assert_thread();
    thread_info_t *info = &thread_info[get_thread_id().threadnum];
    cross_thread_mutex_t::acq_t mutex_acq(&info->mutex);
    size_t res = info->evicters.erase(evicter);
    guarantee(res == 1);
}

void alt_cache_balancer_t::notify_access() {
    __sync_add_and_fetch(&thread_info[get_thread_id().threadnum].access_count, 1);
}

void alt_cache_balancer_t::on_ring() {
    assert_thread();

    // Determine if we should do a rebalance, either:
    //  1. At least rebalance_timeout_ms milliseconds have passed
    //  2. At least access_count_threshold accesses have occurred
    // since the last rebalance.
    microtime_t now = current_microtime();
    if (last_rebalance_time + (rebalance_timeout_ms * 1000) > now) {
        uint64_t total_accesses = 0;
        for (size_t i = 0; i < thread_info.size(); ++i) {
            __sync_synchronize();
            total_accesses += thread_info[i].access_count;
            __sync_synchronize();
        }

        if (total_accesses < rebalance_access_count_threshold) {
            return;
        }
    }

    last_rebalance_time = now;

    // Can't block in this callback, spawn a new coroutine
    // Using this coro_pool, we only have one rebalance going at once
    pool_queue.give_value(alt_cache_balancer_dummy_value_t());
}

void alt_cache_balancer_t::coro_pool_callback(alt_cache_balancer_dummy_value_t, UNUSED signal_t *interruptor) {
    assert_thread();
    scoped_array_t<std::vector<cache_data_t> > per_thread_data;
    per_thread_data.init(thread_info.size());
    scoped_array_t<uint64_t> cache_in_use;
    cache_in_use.init(thread_info.size());

    // Get cache sizes from shards on each thread
    size_t total_evicters = 0;
    uint64_t total_bytes_loaded = 0;

    for (size_t i = 0; i < thread_info.size(); ++i) {
        std::set<alt::evicter_t*> *current_evicters = &thread_info[i].evicters;

        cross_thread_mutex_t::acq_t mutex_acq(&thread_info[i].mutex);

        per_thread_data[i].reserve(current_evicters->size());
        total_evicters += current_evicters->size();

        for (auto j = current_evicters->begin(); j != current_evicters->end(); ++j) {
            cache_data_t data(*j);
            per_thread_data[i].push_back(data);
            total_bytes_loaded += data.bytes_loaded;
        }
    }

    // Calculate new cache sizes
    if (total_cache_size > 0 && total_evicters > 0) {
        uint64_t total_new_sizes = 0;

        for (size_t i = 0; i < per_thread_data.size(); ++i) {
            for (size_t j = 0; j < per_thread_data[i].size(); ++j) {
                cache_data_t *data = &per_thread_data[i][j];

                double temp = data->old_size;
                temp /= static_cast<double>(total_cache_size);
                temp *= static_cast<double>(total_bytes_loaded);

                int64_t new_size = data->bytes_loaded;
                new_size -= static_cast<int64_t>(temp);
                new_size += data->old_size;
                new_size = std::max<int64_t>(new_size, 0);

                data->new_size = new_size;
                total_new_sizes += new_size;
            }
        }

        // Distribute any rounding error across shards
        int64_t extra_bytes = total_cache_size - total_new_sizes;
        while (extra_bytes != 0) {
            int64_t delta = extra_bytes / static_cast<int64_t>(total_evicters);
            if (delta == 0) {
                delta = ((extra_bytes < 0) ? -1 : 1);
            }
            for (size_t i = 0; i < per_thread_data.size() && extra_bytes != 0; ++i) {
                for (size_t j = 0; j < per_thread_data[i].size() && extra_bytes != 0; ++j) {
                    cache_data_t *data = &per_thread_data[i][j];

                    // Avoid underflow
                    if (static_cast<int64_t>(data->new_size) + delta >= 0) {
                        data->new_size += delta;
                        extra_bytes -= delta;
                    } else {
                        extra_bytes += data->new_size;
                        data->new_size = 0;
                    }
                }
            }
        }

        // Send new cache sizes to each thread
        pmap(per_thread_data.size(),
             std::bind(&alt_cache_balancer_t::apply_rebalance_to_thread,
                       this, ph::_1, &per_thread_data, &cache_in_use));

        // We only allow read_ahead to be ok until it has been not ok
        if (read_ahead_ok) {
            // Update read_ahead_ok based on how much cache is in use
            uint64_t cache_usage = 0;
            for (size_t i = 0; i < cache_in_use.size(); ++i) {
                cache_usage += cache_in_use[i];
            }

            read_ahead_ok = (cache_usage * read_ahead_ratio_denominator) <
                            (total_cache_size * read_ahead_ratio_numerator);
        }
    }
}

void alt_cache_balancer_t::apply_rebalance_to_thread(int index,
        scoped_array_t<std::vector<cache_data_t> > *new_sizes,
        scoped_array_t<uint64_t> *cache_in_use) {
    on_thread_t rethreader((threadnum_t(index)));

    // No need to lock the thread_info's mutex since a new rebalance cannot run
    // while we are in here
    std::set<alt::evicter_t *> *evicters = &thread_info[index].evicters;
    std::vector<cache_data_t> *sizes = &(*new_sizes)[index];
    uint64_t *total_cache_usage = &(*cache_in_use)[index];

    *total_cache_usage = 0;

    ASSERT_NO_CORO_WAITING;
    for (auto it = sizes->begin(); it != sizes->end(); ++it) {
        // Make sure the evicter still exists
        if (evicters->find(it->evicter) != evicters->end()) {
            it->evicter->update_memory_limit(it->new_size);

            // Set the cache-in-use so we can calculate if read-ahead is ok
            *total_cache_usage += it->evicter->in_memory_size();
        }
    }

    // Clear the number of accesses for this thread
    thread_info[index].access_count = 0;
}

