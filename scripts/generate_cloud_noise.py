#!/usr/bin/env python3
"""
Generate 3D cloud noise texture for volumetric cloud rendering.
Pre-generates a 128^3 Perlin-Worley noise texture to avoid runtime cost.
"""

import numpy as np
import struct
import sys
from pathlib import Path

def hash_float(x):
    """Simple hash function matching C++ implementation."""
    return abs(np.sin(x * 12.9898 + 78.233) * 43758.5453) % 1.0

def perlin_noise_3d(x, y, z):
    """Simplified 3D Perlin noise matching C++ implementation."""
    xi = np.floor(x).astype(int) & 255
    yi = np.floor(y).astype(int) & 255
    zi = np.floor(z).astype(int) & 255
    
    xf = x - np.floor(x)
    yf = y - np.floor(y)
    zf = z - np.floor(z)
    
    # Smoothstep interpolation
    u = xf * xf * (3.0 - 2.0 * xf)
    v = yf * yf * (3.0 - 2.0 * yf)
    w = zf * zf * (3.0 - 2.0 * zf)
    
    # Hash-based gradients
    n000 = hash_float(xi + yi * 57 + zi * 113)
    n001 = hash_float(xi + yi * 57 + (zi + 1) * 113)
    n010 = hash_float(xi + (yi + 1) * 57 + zi * 113)
    n011 = hash_float(xi + (yi + 1) * 57 + (zi + 1) * 113)
    n100 = hash_float((xi + 1) + yi * 57 + zi * 113)
    n101 = hash_float((xi + 1) + yi * 57 + (zi + 1) * 113)
    n110 = hash_float((xi + 1) + (yi + 1) * 57 + zi * 113)
    n111 = hash_float((xi + 1) + (yi + 1) * 57 + (zi + 1) * 113)
    
    # Trilinear interpolation
    nx00 = n000 * (1 - u) + n100 * u
    nx01 = n001 * (1 - u) + n101 * u
    nx10 = n010 * (1 - u) + n110 * u
    nx11 = n011 * (1 - u) + n111 * u
    
    nxy0 = nx00 * (1 - v) + nx10 * v
    nxy1 = nx01 * (1 - v) + nx11 * v
    
    return nxy0 * (1 - w) + nxy1 * w

def worley_noise_3d(x, y, z):
    """Simplified Worley (cellular) noise matching C++ implementation."""
    cell_x = np.floor(x)
    cell_y = np.floor(y)
    cell_z = np.floor(z)
    
    frac_x = x - cell_x
    frac_y = y - cell_y
    frac_z = z - cell_z
    
    min_dist = 1.0
    
    # Check neighboring cells
    for dz in [-1, 0, 1]:
        for dy in [-1, 0, 1]:
            for dx in [-1, 0, 1]:
                neighbor_x = cell_x + dx
                neighbor_y = cell_y + dy
                neighbor_z = cell_z + dz
                
                # Hash to get feature point
                hash_val = hash_float(neighbor_x * 374761393.0 + neighbor_y * 668265263.0 + neighbor_z * 1274126177.0)
                feature_x = hash_val
                feature_y = hash_float(hash_val * 2.0)
                feature_z = hash_float(hash_val * 3.0)
                
                # Distance to feature point
                diff_x = (neighbor_x + feature_x) - x
                diff_y = (neighbor_y + feature_y) - y
                diff_z = (neighbor_z + feature_z) - z
                dist = np.sqrt(diff_x*diff_x + diff_y*diff_y + diff_z*diff_z)
                
                min_dist = min(min_dist, dist)
    
    return 1.0 - min_dist  # Invert so higher = more solid

def generate_3d_noise(size):
    """Generate 3D Perlin-Worley hybrid noise texture."""
    print(f"Generating {size}^3 cloud noise texture...")
    
    noise_data = np.zeros((size, size, size), dtype=np.uint8)
    
    total_voxels = size * size * size
    progress_interval = total_voxels // 20  # Update every 5%
    
    for z in range(size):
        for y in range(size):
            for x in range(size):
                idx = x + y * size + z * size * size
                if idx % progress_interval == 0:
                    progress = (idx / total_voxels) * 100
                    print(f"Progress: {progress:.0f}%")
                
                # Normalized coordinates
                nx = x / size
                ny = y / size
                nz = z / size
                
                # Perlin noise component
                perlin = perlin_noise_3d(nx * 4.0, ny * 4.0, nz * 4.0)
                
                # Worley noise component
                worley = worley_noise_3d(nx * 2.0, ny * 2.0, nz * 2.0)
                
                # Combine (Perlin-Worley hybrid)
                noise = perlin * 0.6 + worley * 0.4
                noise = np.clip(noise, 0.0, 1.0)
                
                noise_data[z, y, x] = int(noise * 255)
    
    print("Generation complete!")
    return noise_data

def save_binary_texture(data, output_path):
    """Save noise data as raw binary file with header."""
    size = data.shape[0]
    
    with open(output_path, 'wb') as f:
        # Write header: "CN3D" magic + size as uint32
        f.write(b'CN3D')
        f.write(struct.pack('I', size))
        
        # Write raw data
        f.write(data.tobytes())
    
    print(f"Saved texture to: {output_path}")
    print(f"File size: {output_path.stat().st_size / (1024*1024):.2f} MB")

def main():
    size = 128  # Must match EngineParameters::Clouds::NOISE_TEXTURE_SIZE
    
    # Generate noise
    noise_data = generate_3d_noise(size)
    
    # Save to assets/textures/
    script_dir = Path(__file__).parent
    project_root = script_dir.parent
    output_path = project_root / "assets" / "textures" / "cloud_noise_3d.bin"
    output_path.parent.mkdir(parents=True, exist_ok=True)
    
    save_binary_texture(noise_data, output_path)
    
    print("\n✓ Cloud noise texture generated successfully!")
    print(f"  Add this file to your assets copy in CMakeLists.txt")

if __name__ == "__main__":
    main()
