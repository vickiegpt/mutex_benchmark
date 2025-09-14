# mutex_benchmark

Benchmarking different mutex libraries and implementations

## Installation

Install the Boost library [here](https://www.boost.org/doc/libs/1_53_0/doc/html/bbv2/installation.html).
Install nsync [here](https://github.com/google/nsync).

Install `pandas` and `matplotlib` for Python:

```python
pip install matplotlib pandas
```

## Experiment Running

### Max Benchmark

Series of commands to run experiments

This command runs each iteration for 5 seconds with an expected total of 8 threads 3 times (8 5 3)
It iterates with increasing threads starting at thread 1 up to 8 with an interval of 1 thread (--iter-threads 1 8 1)
with the max benchmark (-bench max) with the software_cxl lock (-s software_cxl) set defined in constants file.

```python
python3 -m scripts.main 8 5 3 --iter-threads 1 8 1 --bench max -s software_cxl  
```


This command runs with the max benchmark (-bench max) with the software_cxl lock (-s hardware_cxl) set defined in constants file.

```python
python3 -m scripts.main 8 5 3 --iter-threads 1 8 1 --bench max -s hardware_cxl  
```

To graph both sets of experiments together with replacing the underlying data files
```python
python3 -m scripts.main 8 5 3 --iter-threads 1 8 1 --bench max -s combined_cxl --skip-experiment  
```

If the -hcxl flag is added to the run command, the allocation function will be changed from malloc to mmap, mbind to nodemask = 1UL. This can be modified in the lib/utils/cxl_utils.cpp file.
```python
python3 -m scripts.main 8 5 3 --iter-threads 1 8 1 --bench max -s software_cxl  -hcxl
```