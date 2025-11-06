#!/usr/bin/env python3
"""
Block Texture Generator for MMORPG Engine
Generates procedural 32x32 textures for all biome-specific blocks.
Only generates missing textures to allow manual curation.

Usage: python generate_block_textures.py
"""

import os
import sys
from pathlib import Path
import numpy as np
from PIL import Image
import random

# Texture output directory
TEXTURE_DIR = Path(__file__).parent.parent / "assets" / "textures"

# Block definitions organized by biome usage
BLOCK_TEXTURES = {
    # Core terrain blocks (used in multiple biomes)
    "grass": {
        "base_color": (60, 120, 40),
        "variation": 30,
        "noise_scale": 0.3,
        "pattern": "organic",
        "recipe": "Natural grass surface - temperate biomes"
    },
    "dirt": {
        "base_color": (120, 80, 50),
        "variation": 25,
        "noise_scale": 0.4,
        "pattern": "organic",
        "recipe": "Soil and earth - subsurface layer"
    },
    "stone": {
        "base_color": (100, 100, 100),
        "variation": 20,
        "noise_scale": 0.25,
        "pattern": "rocky",
        "recipe": "Common stone - deep layer"
    },
    "sand": {
        "base_color": (220, 190, 120),
        "variation": 15,
        "noise_scale": 0.35,
        "pattern": "grainy",
        "recipe": "Beach sand - desert and tropical biomes"
    },
    "gravel": {
        "base_color": (130, 130, 130),
        "variation": 25,
        "noise_scale": 0.4,
        "pattern": "grainy",
        "recipe": "Loose rock fragments"
    },
    "clay": {
        "base_color": (160, 130, 110),
        "variation": 20,
        "noise_scale": 0.3,
        "pattern": "organic",
        "recipe": "Wet clay deposits"
    },
    "moss": {
        "base_color": (40, 90, 40),
        "variation": 25,
        "noise_scale": 0.35,
        "pattern": "organic",
        "recipe": "Mossy ground cover - forest biomes"
    },
    
    # Ice/Snow biome blocks
    "ice": {
        "base_color": (200, 220, 255),
        "variation": 15,
        "noise_scale": 0.2,
        "pattern": "crystalline",
        "recipe": "Frozen water - snow biome surface"
    },
    "packed_ice": {
        "base_color": (160, 200, 240),
        "variation": 10,
        "noise_scale": 0.15,
        "pattern": "crystalline",
        "recipe": "Dense compressed ice"
    },
    "snow": {
        "base_color": (245, 250, 255),
        "variation": 8,
        "noise_scale": 0.25,
        "pattern": "grainy",
        "recipe": "Fresh snow layer"
    },
    
    # Desert/Beach blocks
    "sandstone": {
        "base_color": (210, 170, 100),
        "variation": 18,
        "noise_scale": 0.28,
        "pattern": "rocky",
        "recipe": "Compressed sand layers - desert stone"
    },
    
    # Stone variants
    "granite": {
        "base_color": (140, 120, 110),
        "variation": 22,
        "noise_scale": 0.22,
        "pattern": "rocky",
        "recipe": "Speckled igneous rock"
    },
    "basalt": {
        "base_color": (60, 60, 65),
        "variation": 18,
        "noise_scale": 0.2,
        "pattern": "rocky",
        "recipe": "Dark volcanic rock"
    },
    "limestone": {
        "base_color": (220, 220, 200),
        "variation": 20,
        "noise_scale": 0.3,
        "pattern": "rocky",
        "recipe": "Sedimentary stone - tropical biome deep layer"
    },
    "marble": {
        "base_color": (240, 240, 245),
        "variation": 15,
        "noise_scale": 0.18,
        "pattern": "crystalline",
        "recipe": "Polished metamorphic stone"
    },
    "obsidian": {
        "base_color": (20, 15, 30),
        "variation": 12,
        "noise_scale": 0.15,
        "pattern": "crystalline",
        "sparkle": True,
        "recipe": "Volcanic glass - rare and strong"
    },
    
    # Volcanic blocks
    "lava_rock": {
        "base_color": (80, 40, 30),
        "variation": 20,
        "noise_scale": 0.25,
        "pattern": "rocky",
        "recipe": "Cooled lava stone"
    },
    "volcanic_ash": {
        "base_color": (70, 70, 75),
        "variation": 15,
        "noise_scale": 0.35,
        "pattern": "grainy",
        "recipe": "Fine volcanic debris"
    },
    "magma": {
        "base_color": (200, 80, 20),
        "variation": 35,
        "noise_scale": 0.2,
        "pattern": "fluid",
        "glow": True,
        "recipe": "Molten rock - glowing hot"
    },
    "lava": {
        "base_color": (255, 100, 0),
        "variation": 40,
        "noise_scale": 0.18,
        "pattern": "fluid",
        "glow": True,
        "recipe": "Liquid lava flow"
    },
    
    # Ore blocks
    "coal": {
        "base_color": (30, 30, 30),
        "variation": 15,
        "noise_scale": 0.2,
        "pattern": "ore",
        "ore_color": (10, 10, 10),
        "recipe": "Carbon ore - fuel source"
    },
    "iron_block": {
        "base_color": (180, 180, 190),
        "variation": 20,
        "noise_scale": 0.15,
        "pattern": "metallic",
        "ore_color": (160, 140, 130),
        "recipe": "Iron ore - common metal"
    },
    "copper_block": {
        "base_color": (184, 115, 51),
        "variation": 20,
        "noise_scale": 0.15,
        "pattern": "metallic",
        "ore_color": (150, 90, 40),
        "recipe": "Copper ore - conductive metal"
    },
    "gold_block": {
        "base_color": (255, 215, 0),
        "variation": 25,
        "noise_scale": 0.15,
        "pattern": "metallic",
        "ore_color": (220, 180, 0),
        "recipe": "Gold ore - precious metal"
    },
    
    # Gemstone/Crystal blocks
    "diamond_block": {
        "base_color": (100, 200, 255),
        "variation": 30,
        "noise_scale": 0.1,
        "pattern": "crystalline",
        "sparkle": True,
        "recipe": "Diamond crystal - hardest material"
    },
    "emerald_block": {
        "base_color": (50, 205, 50),
        "variation": 28,
        "noise_scale": 0.12,
        "pattern": "crystalline",
        "sparkle": True,
        "recipe": "Emerald gemstone - vibrant green"
    },
    "ruby_block": {
        "base_color": (224, 17, 95),
        "variation": 30,
        "noise_scale": 0.12,
        "pattern": "crystalline",
        "sparkle": True,
        "recipe": "Ruby gemstone - deep red"
    },
    "sapphire_block": {
        "base_color": (15, 82, 186),
        "variation": 28,
        "noise_scale": 0.12,
        "pattern": "crystalline",
        "sparkle": True,
        "recipe": "Sapphire gemstone - royal blue"
    },
    "amethyst": {
        "base_color": (153, 102, 204),
        "variation": 30,
        "noise_scale": 0.12,
        "pattern": "crystalline",
        "sparkle": True,
        "recipe": "Amethyst crystal - purple quartz"
    },
    "quartz": {
        "base_color": (230, 230, 240),
        "variation": 18,
        "noise_scale": 0.15,
        "pattern": "crystalline",
        "recipe": "White quartz crystal"
    },
    
    # Colored crystals
    "crystal_blue": {
        "base_color": (80, 150, 255),
        "variation": 25,
        "noise_scale": 0.12,
        "pattern": "crystalline",
        "sparkle": True,
        "recipe": "Blue energy crystal"
    },
    "crystal_green": {
        "base_color": (50, 255, 150),
        "variation": 25,
        "noise_scale": 0.12,
        "pattern": "crystalline",
        "sparkle": True,
        "recipe": "Green energy crystal"
    },
    "crystal_purple": {
        "base_color": (180, 100, 255),
        "variation": 25,
        "noise_scale": 0.12,
        "pattern": "crystalline",
        "sparkle": True,
        "recipe": "Purple energy crystal"
    },
    "crystal_pink": {
        "base_color": (255, 120, 200),
        "variation": 25,
        "noise_scale": 0.12,
        "pattern": "crystalline",
        "sparkle": True,
        "recipe": "Pink energy crystal"
    },
    
    # Special blocks
    "salt_block": {
        "base_color": (240, 240, 250),
        "variation": 10,
        "noise_scale": 0.15,
        "pattern": "crystalline",
        "recipe": "Salt deposits - preservative"
    },
    "mushroom_block": {
        "base_color": (200, 180, 160),
        "variation": 25,
        "noise_scale": 0.3,
        "pattern": "organic",
        "recipe": "Giant mushroom material"
    },
    "coral": {
        "base_color": (255, 127, 80),
        "variation": 30,
        "noise_scale": 0.25,
        "pattern": "organic",
        "recipe": "Ocean coral structure"
    },
    
    # Fluids
    "water": {
        "base_color": (40, 80, 180),
        "variation": 20,
        "noise_scale": 0.2,
        "pattern": "fluid",
        "recipe": "Water - essential liquid"
    }
}


def perlin_noise_2d(width, height, scale=0.1, octaves=4):
    """Generate 2D Perlin-like noise using numpy"""
    def interpolate(a, b, x):
        ft = x * np.pi
        f = (1 - np.cos(ft)) * 0.5
        return a * (1 - f) + b * f
    
    # Start with random gradients
    noise = np.zeros((height, width))
    
    for octave in range(octaves):
        freq = 2 ** octave
        amp = 1 / (2 ** octave)
        
        # Generate random gradient field
        grid_size = max(2, int(width / (10 * scale * freq)))
        gradients = np.random.rand(grid_size + 2, grid_size + 2) * 2 - 1
        
        # Sample and interpolate
        for y in range(height):
            for x in range(width):
                grid_x = x / width * grid_size
                grid_y = y / height * grid_size
                
                x0, y0 = int(grid_x), int(grid_y)
                x1, y1 = x0 + 1, y0 + 1
                
                sx = grid_x - x0
                sy = grid_y - y0
                
                n0 = gradients[y0, x0]
                n1 = gradients[y0, x1]
                ix0 = interpolate(n0, n1, sx)
                
                n0 = gradients[y1, x0]
                n1 = gradients[y1, x1]
                ix1 = interpolate(n0, n1, sx)
                
                noise[y, x] += interpolate(ix0, ix1, sy) * amp
    
    # Normalize to 0-1
    noise = (noise - noise.min()) / (noise.max() - noise.min())
    return noise


def generate_organic_pattern(width, height, base_color, variation, noise_scale):
    """Generate organic-looking texture (grass, dirt)"""
    img = np.zeros((height, width, 3), dtype=np.uint8)
    noise = perlin_noise_2d(width, height, noise_scale, octaves=4)
    
    for y in range(height):
        for x in range(width):
            n = noise[y, x]
            r = int(np.clip(base_color[0] + (n - 0.5) * variation * 2, 0, 255))
            g = int(np.clip(base_color[1] + (n - 0.5) * variation * 2, 0, 255))
            b = int(np.clip(base_color[2] + (n - 0.5) * variation * 2, 0, 255))
            img[y, x] = [r, g, b]
    
    return img


def generate_rocky_pattern(width, height, base_color, variation, noise_scale):
    """Generate rocky/stone texture with cracks"""
    img = np.zeros((height, width, 3), dtype=np.uint8)
    noise = perlin_noise_2d(width, height, noise_scale, octaves=5)
    crack_noise = perlin_noise_2d(width, height, 0.5, octaves=2)
    
    for y in range(height):
        for x in range(width):
            n = noise[y, x]
            crack = crack_noise[y, x]
            
            # Add cracks
            if crack > 0.85:
                darkness = 0.3
            else:
                darkness = 1.0
            
            r = int(np.clip(base_color[0] * darkness + (n - 0.5) * variation * 2, 0, 255))
            g = int(np.clip(base_color[1] * darkness + (n - 0.5) * variation * 2, 0, 255))
            b = int(np.clip(base_color[2] * darkness + (n - 0.5) * variation * 2, 0, 255))
            img[y, x] = [r, g, b]
    
    return img


def generate_grainy_pattern(width, height, base_color, variation, noise_scale):
    """Generate grainy texture (sand)"""
    img = np.zeros((height, width, 3), dtype=np.uint8)
    noise = perlin_noise_2d(width, height, noise_scale, octaves=6)
    
    # Add fine grain
    grain = np.random.rand(height, width) * 0.2
    
    for y in range(height):
        for x in range(width):
            n = noise[y, x] + grain[y, x]
            r = int(np.clip(base_color[0] + (n - 0.5) * variation * 2, 0, 255))
            g = int(np.clip(base_color[1] + (n - 0.5) * variation * 2, 0, 255))
            b = int(np.clip(base_color[2] + (n - 0.5) * variation * 2, 0, 255))
            img[y, x] = [r, g, b]
    
    return img


def generate_crystalline_pattern(width, height, base_color, variation, noise_scale, sparkle=False):
    """Generate crystalline texture (ice, diamond, salt)"""
    img = np.zeros((height, width, 3), dtype=np.uint8)
    noise = perlin_noise_2d(width, height, noise_scale, octaves=3)
    
    for y in range(height):
        for x in range(width):
            n = noise[y, x]
            
            # Sharp transitions for crystal facets
            if n > 0.7:
                brightness = 1.3
            elif n > 0.4:
                brightness = 1.0
            else:
                brightness = 0.8
            
            # Add sparkle for diamonds
            if sparkle and random.random() > 0.95:
                brightness = 1.8
            
            r = int(np.clip(base_color[0] * brightness, 0, 255))
            g = int(np.clip(base_color[1] * brightness, 0, 255))
            b = int(np.clip(base_color[2] * brightness, 0, 255))
            img[y, x] = [r, g, b]
    
    return img


def generate_metallic_pattern(width, height, base_color, variation, noise_scale, ore_color):
    """Generate metallic ore texture"""
    img = np.zeros((height, width, 3), dtype=np.uint8)
    noise = perlin_noise_2d(width, height, noise_scale, octaves=3)
    vein_noise = perlin_noise_2d(width, height, 0.15, octaves=2)
    
    for y in range(height):
        for x in range(width):
            n = noise[y, x]
            vein = vein_noise[y, x]
            
            # Metallic shine
            if n > 0.6:
                brightness = 1.2
            else:
                brightness = 0.9
            
            # Ore veins
            if vein > 0.6:
                r = int(np.clip(ore_color[0] * brightness, 0, 255))
                g = int(np.clip(ore_color[1] * brightness, 0, 255))
                b = int(np.clip(ore_color[2] * brightness, 0, 255))
            else:
                r = int(np.clip(base_color[0] * brightness + (n - 0.5) * variation, 0, 255))
                g = int(np.clip(base_color[1] * brightness + (n - 0.5) * variation, 0, 255))
                b = int(np.clip(base_color[2] * brightness + (n - 0.5) * variation, 0, 255))
            
            img[y, x] = [r, g, b]
    
    return img


def generate_ore_pattern(width, height, base_color, variation, noise_scale, ore_color):
    """Generate coal/ore texture"""
    img = np.zeros((height, width, 3), dtype=np.uint8)
    noise = perlin_noise_2d(width, height, noise_scale, octaves=4)
    
    for y in range(height):
        for x in range(width):
            n = noise[y, x]
            
            # Use ore color for base with some variation
            r = int(np.clip(ore_color[0] + (n - 0.5) * variation * 2, 0, 255))
            g = int(np.clip(ore_color[1] + (n - 0.5) * variation * 2, 0, 255))
            b = int(np.clip(ore_color[2] + (n - 0.5) * variation * 2, 0, 255))
            img[y, x] = [r, g, b]
    
    return img


def generate_fluid_pattern(width, height, base_color, variation, noise_scale, glow=False):
    """Generate fluid/water texture"""
    img = np.zeros((height, width, 3), dtype=np.uint8)
    noise = perlin_noise_2d(width, height, noise_scale, octaves=4)
    wave_noise = perlin_noise_2d(width, height, 0.4, octaves=2)
    
    for y in range(height):
        for x in range(width):
            n = noise[y, x]
            wave = wave_noise[y, x]
            
            # Wave highlights
            if glow:
                # Lava/magma glow effect
                brightness = 0.8 + wave * 0.5 + n * 0.3
            else:
                # Water wave effect
                brightness = 0.9 + wave * 0.3
            
            r = int(np.clip(base_color[0] * brightness + (n - 0.5) * variation, 0, 255))
            g = int(np.clip(base_color[1] * brightness + (n - 0.5) * variation, 0, 255))
            b = int(np.clip(base_color[2] * brightness + (n - 0.5) * variation, 0, 255))
            img[y, x] = [r, g, b]
    
    return img


def generate_texture(block_name, config, width=32, height=32):
    """Generate a single texture based on configuration"""
    pattern = config["pattern"]
    base_color = config["base_color"]
    variation = config["variation"]
    noise_scale = config["noise_scale"]
    
    if pattern == "organic":
        img_array = generate_organic_pattern(width, height, base_color, variation, noise_scale)
    elif pattern == "rocky":
        img_array = generate_rocky_pattern(width, height, base_color, variation, noise_scale)
    elif pattern == "grainy":
        img_array = generate_grainy_pattern(width, height, base_color, variation, noise_scale)
    elif pattern == "crystalline":
        sparkle = config.get("sparkle", False)
        img_array = generate_crystalline_pattern(width, height, base_color, variation, noise_scale, sparkle)
    elif pattern == "metallic":
        ore_color = config["ore_color"]
        img_array = generate_metallic_pattern(width, height, base_color, variation, noise_scale, ore_color)
    elif pattern == "ore":
        ore_color = config["ore_color"]
        img_array = generate_ore_pattern(width, height, base_color, variation, noise_scale, ore_color)
    elif pattern == "fluid":
        glow = config.get("glow", False)
        img_array = generate_fluid_pattern(width, height, base_color, variation, noise_scale, glow)
    else:
        # Default to organic
        img_array = generate_organic_pattern(width, height, base_color, variation, noise_scale)
    
    return Image.fromarray(img_array, 'RGB')


def main():
    """Main generation function"""
    # Ensure texture directory exists
    TEXTURE_DIR.mkdir(parents=True, exist_ok=True)
    
    print(f"🎨 Block Texture Generator")
    print(f"📁 Output directory: {TEXTURE_DIR}")
    print(f"📦 Total blocks to process: {len(BLOCK_TEXTURES)}")
    print()
    
    generated = 0
    skipped = 0
    
    for block_name, config in BLOCK_TEXTURES.items():
        output_path = TEXTURE_DIR / f"{block_name}.png"
        
        # Skip if texture already exists
        if output_path.exists():
            print(f"⏭️  Skipping {block_name}.png (already exists)")
            skipped += 1
            continue
        
        print(f"✨ Generating {block_name}.png...")
        
        # Generate texture
        texture = generate_texture(block_name, config)
        
        # Save to file
        texture.save(output_path, "PNG")
        generated += 1
    
    print()
    print(f"✅ Generation complete!")
    print(f"   Generated: {generated} textures")
    print(f"   Skipped: {skipped} textures (already exist)")
    print(f"   Total: {len(BLOCK_TEXTURES)} blocks")


if __name__ == "__main__":
    try:
        main()
    except ImportError as e:
        print(f"❌ Error: Missing dependency - {e}")
        print()
        print("Please install required packages:")
        print("  pip install pillow numpy")
        sys.exit(1)
    except Exception as e:
        print(f"❌ Error: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)
