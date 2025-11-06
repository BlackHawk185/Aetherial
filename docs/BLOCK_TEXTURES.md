# Block Textures for Biome System

## Required Textures by Biome

### Grassland Biome
- **Surface**: `grass.png` - Green grassy terrain
- **Subsurface**: `dirt.png` - Brown soil
- **Deep**: `stone.png` - Gray stone
- **Ore**: `coal.png` - Dark coal deposits

### Forest Biome
- **Surface**: `grass.png` - Green grassy terrain
- **Subsurface**: `dirt.png` - Brown soil
- **Deep**: `stone.png` - Gray stone
- **Ore**: `coal.png` - Dark coal deposits

### Desert Biome
- **Surface**: `sand.png` - Sandy yellow terrain
- **Subsurface**: `sand.png` - More sand
- **Deep**: `limestone.png` - Off-white limestone
- **Ore**: `gold_block.png` - Golden ore

### Snow Biome
- **Surface**: `ice.png` - Light blue ice
- **Subsurface**: `dirt.png` - Frozen soil
- **Deep**: `stone.png` - Gray stone
- **Ore**: `iron_block.png` - Metallic gray iron

### Volcanic Biome
- **Surface**: `stone.png` - Dark rock
- **Subsurface**: `stone.png` - Stone
- **Deep**: `stone.png` - Stone
- **Ore**: `coal.png` - Abundant coal

### Crystal Biome
- **Surface**: `diamond_block.png` - Cyan crystalline
- **Subsurface**: `stone.png` - Stone
- **Deep**: `stone.png` - Stone
- **Ore**: `diamond_block.png` - Precious diamonds

### Tropical Biome
- **Surface**: `grass.png` - Lush grass
- **Subsurface**: `sand.png` - Sandy beach layer
- **Deep**: `limestone.png` - Limestone
- **Ore**: `copper_block.png` - Copper orange

### Barren Biome
- **Surface**: `stone.png` - Rocky terrain
- **Subsurface**: `stone.png` - Stone
- **Deep**: `stone.png` - Stone
- **Ore**: `iron_block.png` - Iron deposits

## Additional Blocks
- `water.png` - For water features (future)
- `salt_block.png` - For salt deposits (future)

## Texture Generation

Run the generation script:
```bash
python scripts/generate_block_textures.py
```

The script will:
- Generate all missing textures at 32x32 resolution
- Skip existing textures (allowing manual curation)
- Use procedural generation with appropriate patterns for each block type

## Texture Patterns

- **Organic**: Grass, dirt (natural variation)
- **Rocky**: Stone, limestone (cracks and weathering)
- **Grainy**: Sand (fine grain texture)
- **Crystalline**: Ice, diamond, salt (faceted surfaces)
- **Metallic**: Iron, gold, copper (metallic shine)
- **Ore**: Coal (dark mineral deposits)
- **Fluid**: Water (wave patterns)

## Workflow

1. Generate initial textures: `python scripts/generate_block_textures.py`
2. Review and edit textures manually in `assets/textures/`
3. Keep textures you like, delete ones you want to regenerate
4. Run script again to regenerate only missing textures
