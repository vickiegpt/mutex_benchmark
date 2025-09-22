#!/bin/bash
echo $$ | sudo tee /sys/fs/cgroup/a/cgroup.procs
# ./memeater 10
python3 -m scripts.main 64 0.1 1 --iter-threads 1 64 1 --bench max -s combined_cxl 
