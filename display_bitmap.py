#!/usr/bin/env python3
import sys
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

def visualize_bitmap(path, width=256):
    with open(path, 'rb') as f:
        data = f.read(65536)

    arr = np.frombuffer(data, dtype=np.uint8)
    height = len(arr) // width
    arr = arr[:height * width].reshape(height, width)

    plt.figure(figsize=(14, 6))

    plt.subplot(1, 2, 1)
    plt.imshow(arr, aspect='auto', cmap='viridis', interpolation='nearest')
    plt.colorbar(label='Hit count')
    plt.title('Coverage bitmap heatmap')
    plt.xlabel('Byte offset (mod 256)')
    plt.ylabel('Row (x256)')

    plt.subplot(1, 2, 2)
    nonzero = arr[arr > 0].flatten()
    plt.hist(nonzero, bins=32, color='orange', edgecolor='black')
    plt.title(f'Hit count distribution ({len(nonzero)} non-zero bytes)')
    plt.xlabel('Hit count')
    plt.ylabel('Frequency')
    plt.gca().xaxis.set_major_locator(plt.MaxNLocator(integer=True))
    plt.gca().yaxis.set_major_locator(plt.MaxNLocator(integer=True))


    plt.tight_layout()
    plt.savefig('/tmp/bitmap.png', dpi=400)
    print("Saved to /tmp/bitmap.png")
    print(f"Non-zero bytes: {len(nonzero)} / {len(arr.flatten())}")
    print(f"Max hit count: {arr.max()}")

if __name__ == '__main__':
    visualize_bitmap(sys.argv[1] if len(sys.argv) > 1 else 'bitmap.bin')