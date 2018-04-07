#!/bin/bash
#
# Migrate zookeeper using minos.
#

if [ $# -le 2 ]; then
  echo "USAGE: $0 <cluster-name> <cluster-meta-list> <target-zookeeper-hosts>"
  echo
  echo "For example:"
  echo "  $0 onebox 127.0.0.1:34601,127.0.0.1:34602 127.0.0.1:2181"
  echo
  exit -1
fi

cluster=$1
meta_list=$2
target_zk=$3

pwd="$( cd "$( dirname "$0"  )" && pwd )"
shell_dir="$( cd $pwd/.. && pwd )"
minos_config_dir=$(dirname $MINOS_CONFIG_FILE)/xiaomi-config/conf/pegasus
minos_client_dir=/home/work/pegasus/infra/minos/client
cd $shell_dir

minos_config=$minos_config_dir/pegasus-${cluster}.cfg
if [ ! -f $minos_config ]; then
  echo "ERROR: minos config \"$minos_config\" not found"
  exit -1
fi

minos_client=$minos_client_dir/deploy
if [ ! -f $minos_client ]; then
  echo "ERROR: minos client \"$minos_client\" not found"
  exit -1
fi

if [ `cat $minos_config | grep " recover_from_replica_server = " | wc -l` -ne 1 ] ; then
  echo "ERROR: no [recover_from_replica_server] entry in minos config \"$minos_config\""
  exit -1
fi
if [ `cat $minos_config | grep " hosts_list = " | wc -l` -ne 1 ] ; then
  echo "ERROR: no [hosts_list] entry in minos config \"$minos_config\""
  exit -1
fi

low_version_count=`echo server_info | ./run.sh shell --cluster $meta_list 2>&1 | grep 'Pegasus Server' | grep -v 'Pegasus Server 1.5' | wc -l`
if [ $low_version_count -gt 0 ]; then
  echo "ERROR: cluster should upgrade to v1.5.0"
  exit -1
fi

echo ">>>> Backuping app list..."
echo "ls -o ${cluster}.apps" | ./run.sh shell --cluster $meta_list &>/dev/null
if [ `cat ${cluster}.apps | wc -l` -eq 0 ]; then
  echo "ERROR: backup app list failed"
  exit -1
fi
cat ${cluster}.apps

echo ">>>> Backuping node list..."
echo "nodes -d -o ${cluster}.nodes" | ./run.sh shell --cluster $meta_list &>/dev/null
if [ `cat ${cluster}.nodes | wc -l` -eq 0 ]; then
  echo "ERROR: backup node list failed"
  exit -1
fi
if [ `grep UNALIVE ${cluster}.nodes | wc -l` -gt 0 ]; then
  echo "ERROR: unalive nodes found in \"${cluster}.nodes\""
  exit -1
fi
cat ${cluster}.nodes | grep " ALIVE" | awk '{print $1}' >${cluster}.recover.nodes
if [ `cat ${cluster}.recover.nodes | wc -l` -eq 0 ]; then
  echo "ERROR: no node found in \"${cluster}.recover.nodes\""
  exit -1
fi

echo ">>>> Generating recover config..."
cp -f $minos_config ${minos_config}.bak
sed -i "s/ recover_from_replica_server = .*/ recover_from_replica_server = true/" $minos_config
sed -i "s/ hosts_list = .*/ hosts_list = ${target_zk}/" $minos_config

echo ">>>> Stopping all meta-servers..."
cd $minos_client_dir
./deploy stop pegasus $cluster --skip_confirm --job meta 2>&1 | tee /tmp/$UID.pegasus.migrate_zookeeper.minos.stop.meta.all
cd $shell_dir

echo ">>>> Sleep for 15 seconds..."
sleep 15

function rolling_update_meta()
{
  task_id=$1
  cd $minos_client_dir
  ./deploy rolling_update pegasus $cluster --skip_confirm --time_interval 10 --update_config --job meta --task $task_id 2>&1 | tee /tmp/$UID.pegasus.migrate_zookeeper.minos.rolling.meta.$task_id
  if [ `cat /tmp/$UID.pegasus.migrate_zookeeper.minos.rolling.meta.$task_id | grep "Start task $task_id of meta .* success" | wc -l` -ne 1 ]; then
    echo "ERROR: rolling update meta-servers task $task_id failed, refer to /tmp/$UID.pegasus.migrate_zookeeper.minos.rolling.meta.$task_id"
    cd $shell_dir
    return 1
  fi
  cd $shell_dir
  return 0
}

function undo()
{
  echo ">>>> Undo..."
  mv -f ${minos_config}.bak $minos_config
  rolling_update_meta 0
  rolling_update_meta 1
}

echo ">>>> Rolling update meta-server task 0..."
rolling_update_meta 0
if [ $? -ne 0 ]; then
  undo
  exit -1
fi

echo ">>>> Sending recover command..."
echo "recover -f ${cluster}.recover.nodes" | ./run.sh shell --cluster $meta_list &>/tmp/$UID.pegasus.migrate_zookeeper.shell.recover
cat /tmp/$UID.pegasus.migrate_zookeeper.shell.recover
if [ `cat /tmp/$UID.pegasus.migrate_zookeeper.shell.recover | grep "Recover result: ERR_OK" | wc -l` -ne 1 ]; then
  echo "ERROR: recover failed, refer to /tmp/$UID.pegasus.migrate_zookeeper.shell.recover"
  undo
  exit 1
fi

echo ">>>> Checking recover result, refer to /tmp/$UID.pegasus.migrate_zookeeper.diff..."
awk '{print $1,$2,$3}' ${cluster}.nodes >/tmp/$UID.pegasus.migrate_zookeeper.diff.old
while true
do
  rm -f /tmp/$UID.pegasus.migrate_zookeeper.shell.nodes
  echo "nodes -d -o /tmp/$UID.pegasus.migrate_zookeeper.shell.nodes" | ./run.sh shell --cluster $meta_list &>/tmp/$UID.pegasus.migrate_zookeeper.shell.nodes.log
  if [ `cat /tmp/$UID.pegasus.migrate_zookeeper.shell.nodes | wc -l` -eq 0 ]; then
    echo "ERROR: get node list failed, refer to /tmp/$UID.pegasus.migrate_zookeeper.shell.nodes.log"
    exit -1
  fi
  awk '{print $1,$2,$3}' /tmp/$UID.pegasus.migrate_zookeeper.shell.nodes >/tmp/$UID.pegasus.migrate_zookeeper.diff.new
  diff /tmp/$UID.pegasus.migrate_zookeeper.diff.old /tmp/$UID.pegasus.migrate_zookeeper.diff.new &>/tmp/$UID.pegasus.migrate_zookeeper.diff
  if [ $? -eq 0 ]; then
    break
  fi
  sleep 1
done

echo ">>>> Modifying config..."
sed -i "s/ recover_from_replica_server = .*/ recover_from_replica_server = false/" $minos_config

echo ">>>> Rolling update meta-server task 1..."
rolling_update_meta 1
if [ $? -ne 0 ]; then
  exit -1
fi

echo ">>>> Rolling update meta-server task 0..."
rolling_update_meta 0
if [ $? -ne 0 ]; then
  exit -1
fi

echo ">>>> Querying cluster info..."
echo "cluster_info" | ./run.sh shell --cluster $meta_list

echo "Migrate zookeeper done."

