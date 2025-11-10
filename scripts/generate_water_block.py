import trimesh
import numpy as np

def create_water_block():
    """Create subdivided top-surface-only water block for wave deformation"""
    
    # Configuration
    subdivisions = 4  # 4x4 grid = 16 quads per block
    size = 1.0
    half_size = size / 2.0
    
    # Generate subdivided top surface (Y = +0.5)
    vertices = []
    step = size / subdivisions
    
    # Create grid of vertices on top surface
    for z in range(subdivisions + 1):
        for x in range(subdivisions + 1):
            vert_x = -half_size + x * step
            vert_z = -half_size + z * step
            vert_y = half_size  # Top surface
            vertices.append([vert_x, vert_y, vert_z])
    
    vertices = np.array(vertices, dtype=np.float32)
    
    # Generate faces (two triangles per quad)
    faces = []
    verts_per_row = subdivisions + 1
    
    for z in range(subdivisions):
        for x in range(subdivisions):
            # Indices of quad corners
            top_left = z * verts_per_row + x
            top_right = top_left + 1
            bottom_left = (z + 1) * verts_per_row + x
            bottom_right = bottom_left + 1
            
            # Two triangles (CCW winding for upward normal)
            faces.append([top_left, bottom_left, top_right])
            faces.append([top_right, bottom_left, bottom_right])
    
    faces = np.array(faces, dtype=np.int32)
    
    # Create mesh (top surface only, no sides/bottom)
    mesh = trimesh.Trimesh(vertices=vertices, faces=faces, process=False)
    
    return mesh

def main():
    print("Generating subdivided water block GLB (top surface only, 4x4 grid)...")
    
    # Create the water block mesh
    water_mesh = create_water_block()
    
    # Export as GLB
    import os
    output_path = os.path.join(os.path.dirname(__file__), "..", "assets", "models", "water.glb")
    output_path = os.path.normpath(output_path)
    water_mesh.export(output_path, file_type='glb')
    
    print(f"✓ Water block GLB created: {output_path}")
    print(f"  Vertices: {len(water_mesh.vertices)}")
    print(f"  Faces: {len(water_mesh.faces)}")
    print(f"  Size: 1x1x1 unit cube")
    print(f"  Bounds: {water_mesh.bounds}")

if __name__ == "__main__":
    main()
