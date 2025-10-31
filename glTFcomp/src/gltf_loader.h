#include "../external/pybind11/include/pybind11/numpy.h"
#include <pybind11/pytypes.h>  // for py::dict, py::list, py::str, etc.

namespace py = pybind11;
struct Vertex;
template <typename T>
std::vector<T> NumpyArrayToVector(const py::array_t<T>& arrIn);
std::vector<Vertex> StoreInVertex(
    const std::vector<float>& positions,   
    const std::vector<float>& normals,    
    const std::vector<float>& uvs,         
    const std::vector<uint32_t>& indices);
void ReadBlenderData(const py::dict& mesh_data, const std::string& exportDir, 
    const std::string& filepath, py::list textures, bool useDraco, 
    int dracoLevel, bool useJpg, int jpgLevel, bool zip);
