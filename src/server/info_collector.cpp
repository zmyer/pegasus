// Copyright (c) 2017, Xiaomi, Inc.  All rights reserved.
// This source code is licensed under the Apache License Version 2.0, which
// can be found in the LICENSE file in the root directory of this source tree.

#include "info_collector.h"
#include <pegasus_utils.h>
#include <iostream>
#include <functional>
#include <vector>
#include <chrono>
#include <cstdlib>
#include <iomanip>

#define METRICSNUM 3

using namespace ::dsn;
using namespace ::dsn::replication;

namespace pegasus {
namespace server {

DEFINE_TASK_CODE(LPC_PEGASUS_APP_STAT_TIMER, TASK_PRIORITY_COMMON, ::dsn::THREAD_POOL_DEFAULT)

info_collector::info_collector()
{
    std::vector<::dsn::rpc_address> meta_servers;
    replica_helper::load_meta_servers(meta_servers);

    _meta_servers.assign_group(dsn_group_build("meta-servers"));
    for (auto &ms : meta_servers) {
        dsn_group_add(_meta_servers.group_handle(), ms.c_addr());
    }

    _cluster_name = dsn_config_get_value_string("pegasus.collector", "cluster", "", "cluster name");
    dassert(_cluster_name.size() > 0, "");

    _shell_context.current_cluster_name = _cluster_name;
    _shell_context.meta_list = meta_servers;
    _shell_context.ddl_client = new replication_ddl_client(meta_servers);

    _app_stat_interval_seconds = (uint32_t)dsn_config_get_value_uint64("pegasus.collector",
                                                                       "app_stat_interval_seconds",
                                                                       10, // default value 10s
                                                                       "app stat interval seconds");
}

info_collector::~info_collector()
{
    for (auto kv : _app_stat_counters) {
        delete kv.second;
    }
    dsn_group_destroy(_meta_servers.group_handle());
}

void info_collector::start()
{
    _app_stat_timer_task =
        ::dsn::tasking::enqueue_timer(LPC_PEGASUS_APP_STAT_TIMER,
                                      this,
                                      [this] { on_app_stat(); },
                                      std::chrono::milliseconds(_app_stat_interval_seconds),
                                      0,
                                      std::chrono::minutes(1));
}

void info_collector::stop() { _app_stat_timer_task->cancel(true); }

void info_collector::on_app_stat()
{
    ddebug("start to stat apps");
    std::vector<row_data> rows;
    if (get_app_stat(&_shell_context, "", rows)) {
        std::vector<double> read_qps;
        std::vector<double> write_qps;
        rows.resize(rows.size() + 1);
        read_qps.resize(rows.size());
        write_qps.resize(rows.size());
        row_data &all = rows.back();
        all.row_name = "_all_";
        for (int i = 0; i < rows.size() - 1; ++i) {
            row_data &row = rows[i];
            all.get_qps += row.get_qps;
            all.multi_get_qps += row.multi_get_qps;
            all.put_qps += row.put_qps;
            all.multi_put_qps += row.multi_put_qps;
            all.remove_qps += row.remove_qps;
            all.multi_remove_qps += row.multi_remove_qps;
            all.scan_qps += row.scan_qps;
            read_qps[i] = row.get_qps + row.multi_get_qps + row.scan_qps;
            write_qps[i] = row.put_qps + row.multi_put_qps + row.remove_qps + row.multi_remove_qps;
        }
        read_qps[read_qps.size() - 1] = all.get_qps + all.multi_get_qps + all.scan_qps;
        write_qps[read_qps.size() - 1] =
            all.put_qps + all.multi_put_qps + all.remove_qps + all.multi_remove_qps;
        for (int i = 0; i < rows.size(); ++i) {
            row_data &row = rows[i];
            AppStatCounters *counters = get_app_counters(row.row_name);
            counters->get_qps->set(row.get_qps);
            counters->multi_get_qps->set(row.multi_get_qps);
            counters->put_qps->set(row.put_qps);
            counters->multi_put_qps->set(row.multi_put_qps);
            counters->remove_qps->set(row.remove_qps);
            counters->multi_remove_qps->set(row.multi_remove_qps);
            counters->scan_qps->set(row.scan_qps);
            counters->recent_expire_count->set(row.recent_expire_count);
            counters->recent_filter_count->set(row.recent_filter_count);
            counters->recent_abnormal_count->set(row.recent_abnormal_count);
            counters->storage_mb->set(row.storage_mb);
            counters->storage_count->set(row.storage_count);
            counters->read_qps->set(read_qps[i]);
            counters->write_qps->set(write_qps[i]);
        }
        ddebug("stat apps succeed, app_count = %d, total_read_qps = %" PRId64
               ", total_write_qps = %" PRId64,
               (int)(rows.size() - 1),
               read_qps[read_qps.size() - 1],
               write_qps[read_qps.size() - 1]);
    } else {
        derror("call get_app_stat() failed");
    }
}

info_collector::AppStatCounters *info_collector::get_app_counters(const std::string &app_name)
{
    ::dsn::utils::auto_lock<::dsn::utils::ex_lock_nr> l(_app_stat_counter_lock);
    auto find = _app_stat_counters.find(app_name);
    if (find != _app_stat_counters.end()) {
        return find->second;
    }
    AppStatCounters *counters = new AppStatCounters();
    char buf1[1024];
    char buf2[1024];
#define INIT_COUNER(type)                                                                          \
    do {                                                                                           \
        sprintf(buf1, "app.stat." #type "#%s", app_name.c_str());                                  \
        sprintf(buf2, "statistic the " #type " of app %s", app_name.c_str());                      \
        counters->type.init_app_counter("app.pegasus", buf1, COUNTER_TYPE_NUMBER, buf2);           \
    } while (0)
    INIT_COUNER(get_qps);
    INIT_COUNER(multi_get_qps);
    INIT_COUNER(put_qps);
    INIT_COUNER(multi_put_qps);
    INIT_COUNER(remove_qps);
    INIT_COUNER(multi_remove_qps);
    INIT_COUNER(scan_qps);
    INIT_COUNER(recent_expire_count);
    INIT_COUNER(recent_filter_count);
    INIT_COUNER(recent_abnormal_count);
    INIT_COUNER(storage_mb);
    INIT_COUNER(storage_count);
    INIT_COUNER(read_qps);
    INIT_COUNER(write_qps);
    _app_stat_counters[app_name] = counters;
    return counters;
}
}
} // namespace
