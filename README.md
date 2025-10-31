# glTFComp
A glTF2.0 compression addon for Blender written in C++

In the `addons/` folder you can find the glTFComp.zip file. Install this .zip in Blender and you can find the export menu in `File>Export>compglTF 2.0(glTF)`

If you'd wish to build the project yourself. Open a cmd in the CMakeLists.txt folder and use these 2 commands:   
`cmake -S. -Bbuild -Ax64`   
`cmake --build build -j`