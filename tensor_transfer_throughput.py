import subprocess
import tensorflow as tf
import time
import sys
import os
import signal
import argparse

os.environ["CUDA_VISIBLE_DEVICES"]= ""

FLAGS = None

def default_config():
  optimizer_options = tf.OptimizerOptions(opt_level=tf.OptimizerOptions.L0)
  device_filters = ["/job:ps", "/job:worker/task:%d"%(FLAGS.task)]
  config = tf.ConfigProto(
    device_filters=device_filters,
    graph_options=tf.GraphOptions(optimizer_options=optimizer_options))
  config.log_device_placement = False
  config.allow_soft_placement = False
  return config

def create_graph(ps_device, worker_device):
  """Create graph that keeps variable on ps_device and
  vector of another variable on worker_device"""
  
  tf.reset_default_graph()
  dtype=tf.int32
  params_size = 250*1000*FLAGS.data_mb # 1MB is 250k integers

  with tf.device(ps_device):
    params = tf.get_variable("params", shape=[params_size], dtype=dtype,
                             initializer=tf.zeros_initializer())
  with tf.device(worker_device):
    update = tf.get_variable("update", shape=[params_size], dtype=dtype,
                             initializer=tf.ones_initializer())
    add_op = params.assign(update)
    
  init_op = tf.global_variables_initializer()
  return init_op, add_op

def run_benchmark_distributed(server):
  init_op, add_op = create_graph("/job:ps/task:0", "/job:worker/task:%d"%(FLAGS.task))

  with tf.Session(server.target, config=default_config()) as sess:
    sess.run(init_op)
    # warm up
    for i in xrange(5):
      sess.run(add_op.op)

    start_time = time.time()
    for i in xrange(FLAGS.iters):
      sess.run(add_op.op)
    elapsed_time = time.time() - start_time
    ans = float(FLAGS.iters)*FLAGS.data_mb/elapsed_time
    print "transfer rate: %f MB/s"%(ans)
    
if __name__=='__main__':
  parser = argparse.ArgumentParser()
  parser.add_argument("--job", type=str, default="ps", help="job type")
  parser.add_argument("--task", type=int, default=0, help="task index")
  parser.add_argument("--ps_hosts", type=str, default="127.0.0.1:12222", help="ps hosts")
  parser.add_argument("--worker_hosts", type=str,
                      default="127.0.0.1:22222,127.0.0.1:22223",
                      help="worker hosts")
  parser.add_argument("--data_mb", type=int, default=100, help="tensor data size")
  parser.add_argument("--iters", type=int, default=10, help="iters")
  
  FLAGS, unparsed = parser.parse_known_args()
  
  ps_hosts = FLAGS.ps_hosts.split(",")
  worker_hosts = FLAGS.worker_hosts.split(",")
  cluster = {"ps" : ps_hosts, "worker" : worker_hosts}
  clusterspec = tf.train.ClusterSpec(cluster).as_cluster_def()
  server = tf.train.Server(clusterspec,
                           config=default_config(),
                           job_name=FLAGS.job,
                           task_index=int(FLAGS.task))

  if FLAGS.job == "ps":
    server.join()
  else:
    run_benchmark_distributed(server)
