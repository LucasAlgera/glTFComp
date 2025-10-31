bl_info = {
    "name": "glTF Compression Addon",
    "author": "Lucas Algera",
    "version": (1, 1, 0),
    "blender": (3, 0, 0),
    "location": "File > Export > compglTF 2.0",
    "description": "Export compressed glTF files using glTFCompL",
    "category": "Import-Export",
}

import sys
import os
import bpy
import numpy as np
import bmesh

# try to load the custom C++ module
def load_glTFCompL_module():
    folderpath = os.path.dirname(__file__)
    if folderpath not in sys.path:
        sys.path.insert(0, folderpath)
    import glTFCompL as m
    return m

def extract_data(obj):
    depsgraph = bpy.context.evaluated_depsgraph_get()
    obj_eval = obj.evaluated_get(depsgraph)
    mesh = obj_eval.to_mesh()

    if obj.type == 'MESH':
        try:
            bm = bmesh.new()
            bm.from_mesh(mesh)
            bmesh.ops.triangulate(bm, faces=bm.faces[:])
            bm.to_mesh(mesh)
            bm.free()

            mesh.calc_normals_split()
            mesh.calc_loop_triangles()

            vertices = np.array([(v.co.x, v.co.y, v.co.z) for v in mesh.vertices], dtype=np.float32)
            normals = np.array([loop.normal[:] for loop in mesh.loops], dtype=np.float32)
            indices = np.array([tri.vertices for tri in mesh.loop_triangles], dtype=np.uint32).flatten()

            uvs = None
            if mesh.uv_layers:
                uv_layer = mesh.uv_layers.active.data
                uvs = np.array([(uv.uv.x, uv.uv.y) for uv in uv_layer], dtype=np.float32)

            materials = []
            for mat in mesh.materials:
                if mat:
                    materials.append({
                        'name': mat.name,
                        'base_color': list(mat.diffuse_color[:3]),
                        'metallic': getattr(mat, "metallic", 0.0),
                        'roughness': getattr(mat, "roughness", 0.5),
                    })

            return {
                'vertices': vertices,
                'normals': normals,
                'indices': indices,
                'uvs': uvs,
                'materials': materials,
                'name': obj.name
            }

        finally:
            obj_eval.to_mesh_clear()
    return None

def get_texture_data(obj):
    textures = []
    if not obj.data.materials:
        return textures

    for mat in obj.data.materials:
        if mat and mat.use_nodes:
            for node in mat.node_tree.nodes:
                if node.type == 'TEX_IMAGE' and node.image:
                    tex = {}
                    img = node.image
                    if img.filepath:
                        abs_path = bpy.path.abspath(img.filepath)
                        tex = {
                            'type': 'file',
                            'path': os.path.normpath(abs_path),
                            'name': img.name
                        }
                    elif img.packed_file:
                        width, height = img.size
                        pixels = np.array(img.pixels[:], dtype=np.float32)
                        channels = img.channels
                        pixels = pixels.reshape((height, width, channels))
                        pixels_uint8 = (pixels * 255).astype(np.uint8)
                        tex = {
                            'type': 'packed',
                            'name': img.name,
                            'width': width,
                            'height': height,
                            'channels': channels,
                            'data': pixels_uint8
                        }
                    if tex:
                        textures.append(tex)
    return textures


# For blender export menu I create a Export class
class EXPORT_SCENE_OT_compgltf(bpy.types.Operator):
    """Export scene as compressed glTF 2.0"""
    bl_idname = "export_scene.compgltf"
    bl_label = "compglTF 2.0"
    bl_options = {'PRESET'}

    filename_ext = ".gltf"

    filepath: bpy.props.StringProperty(subtype="FILE_PATH")
    export_selected: bpy.props.BoolProperty(
        name="Export Selected",
        description="Only export selected mesh objects",
        default=False,
    )

    # --- Compression options ---
    use_draco: bpy.props.BoolProperty(
        name="Use Draco Compression",
        description="Enable Draco mesh compression",
        default=True,
    )

    draco_level: bpy.props.IntProperty(
        name="Draco Compression Level",
        description="Compression level (1–9)",
        default=5,
        min=1, max=9,
    )

    use_jpeg: bpy.props.BoolProperty(
        name="Compress Textures (JPEG)",
        description="Convert images to JPEG",
        default=True,
    )

    jpeg_quality: bpy.props.IntProperty(
        name="JPEG Quality",
        description="JPEG quality (1–100)",
        default=75, # default Blender quality
        min=1, max=100,
    )

    use_zip: bpy.props.BoolProperty(
        name="Use ZIP Compression",
        description="Enable ZIP compression for the output file",
        default=False,
    )

    def execute(self, context):
        # Load compression module
        try:
            m = load_glTFCompL_module()
        except ImportError as e:
            self.report({'ERROR'}, f"Could not import glTFCompL: {e}")
            return {'CANCELLED'}

        objects = context.selected_objects if self.export_selected else context.scene.objects
        export_folder = os.path.dirname(self.filepath)
        os.makedirs(export_folder, exist_ok=True)

        for obj in objects:
            # only export meshes
            if obj.type != "MESH":
                continue

            mesh_data = extract_data(obj)
            if not mesh_data:
                continue
            textures = get_texture_data(obj)

            # pass data to compression module
            m.ReadBlenderData(
                mesh_data,
                export_folder,
                self.filepath,
                textures,
                self.use_draco,
                self.draco_level,
                self.use_jpeg,
                self.jpeg_quality,
                self.use_zip,
            )

        self.report({'INFO'}, f"Export complete: {self.filepath}")
        return {'FINISHED'}

    def invoke(self, context, event):
        if not self.filepath:
            self.filepath = bpy.path.abspath("//untitled.gltf")
        context.window_manager.fileselect_add(self)
        return {'RUNNING_MODAL'}

    # draw the UI
    def draw(self, context):
        layout = self.layout
        layout.label(text="compglTF Export Settings")

        box = layout.box()
        box.label(text="Compression Settings:")
        box.prop(self, "use_draco")
        col = box.column()
        col.enabled = self.use_draco
        col.prop(self, "draco_level")

        box.separator()
        box.prop(self, "use_jpeg")
        if self.use_jpeg:
            warning_box = box.box()
            warning_row = warning_box.row()
            warning_row.alert = True
            warning_row.label(text="⚠ JPEG is lossy - some texture data will be lost", icon='ERROR')
        col = box.column()
        col.enabled = self.use_jpeg
        col.prop(self, "jpeg_quality")

        box.separator()
        box.prop(self, "use_zip")


# manditory plugin functions
def menu_func_export(self, context):
    self.layout.operator(
        EXPORT_SCENE_OT_compgltf.bl_idname,
        text="compglTF 2.0 (.glb/.gltf)"
    )

def register():
    bpy.utils.register_class(EXPORT_SCENE_OT_compgltf)
    bpy.types.TOPBAR_MT_file_export.append(menu_func_export)

def unregister():
    bpy.types.TOPBAR_MT_file_export.remove(menu_func_export)
    bpy.utils.unregister_class(EXPORT_SCENE_OT_compgltf)

if __name__ == "__main__":
    register()