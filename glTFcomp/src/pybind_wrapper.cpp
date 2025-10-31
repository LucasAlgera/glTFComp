#include "../external/pybind11/include/pybind11/pybind11.h"
#include <iostream>

#include "comp_func.h"
#include "gltf_loader.h"

PYBIND11_MODULE(glTFCompL, m) {
    m.doc() = "compression plugin";
    m.def("ReadBlenderData", &ReadBlenderData,
        "Export Blender data to glTF with optional Draco compression",
        py::arg("mesh_data"),
        py::arg("exportDir"),
        py::arg("filepath"), 
        py::arg("textures"),         
        py::arg("useDraco"),
        py::arg("dracoLevel"), 
        py::arg("usePng"), 
        py::arg("jpgLevel"), 
        py::arg("zip"));
}