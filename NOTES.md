# Notes

- Pin the worker threads to cores that share an L3 (on AMD, the same CCX/CCD). The atomic claim's cost is a core-to-core cache-line transfer: ~20–50 ns within a shared L3 vs ~80–130 ns across CCDs via the IO die, which is about as much as a normal DRAM read.
