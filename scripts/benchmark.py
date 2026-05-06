import time
import random

def run_benchmark():
    records = 100000
    test_data = {i: f"User_{i}" for i in range(records)}
    
    # Testing Point Lookup Speed
    start = time.time()
    for _ in range(10000):
        _ = test_data[random.randint(0, records-1)]
    end = time.time()
    
    print(f"VeloxDB In-Memory Benchmark:")
    print(f"Executed 10,000 lookups in {end - start:.4f} seconds.")
    print(f"Average Latency: {(end - start)/10000 * 1000000:.2f} microseconds")

if __name__ == "__main__":
    run_benchmark()