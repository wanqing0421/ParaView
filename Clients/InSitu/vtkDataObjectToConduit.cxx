/*=========================================================================

  Program:   ParaView
  Module:    vtkDataObjectToConduit.cxx

  Copyright (c) Kitware, Inc.
  All rights reserved.
  See Copyright.txt or http://www.paraview.org/HTML/Copyright.html for details.

     This software is distributed WITHOUT ANY WARRANTY; without even
     the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
     PURPOSE.  See the above copyright notice for more information.

=========================================================================*/
#include "vtkDataObjectToConduit.h"

#include "vtkAOSDataArrayTemplate.h"
#include "vtkCellData.h"
#include "vtkDataArray.h"
#include "vtkDataObject.h"
#include "vtkDataSet.h"
#include "vtkFieldData.h"
#include "vtkImageData.h"
#include "vtkLogger.h"
#include "vtkPointData.h"
#include "vtkPointSet.h"
#include "vtkPoints.h"
#include "vtkRectilinearGrid.h"
#include "vtkSOADataArrayTemplate.h"
#include "vtkStructuredGrid.h"
#include "vtkTypeFloat32Array.h"
#include "vtkTypeFloat64Array.h"
#include "vtkTypeInt16Array.h"
#include "vtkTypeInt32Array.h"
#include "vtkTypeInt64Array.h"
#include "vtkTypeInt8Array.h"
#include "vtkTypeUInt16Array.h"
#include "vtkTypeUInt32Array.h"
#include "vtkTypeUInt64Array.h"
#include "vtkTypeUInt8Array.h"
#include "vtkUnstructuredGrid.h"

#include <catalyst_conduit.hpp>

//----------------------------------------------------------------------------
vtkDataObjectToConduit::vtkDataObjectToConduit() = default;

//----------------------------------------------------------------------------
vtkDataObjectToConduit::~vtkDataObjectToConduit() = default;

//----------------------------------------------------------------------------
bool vtkDataObjectToConduit::FillConduitNode(
  vtkDataObject* data_object, conduit_cpp::Node& conduit_node)
{
  auto data_set = vtkDataSet::SafeDownCast(data_object);
  if (!data_set)
  {
    vtkLogF(ERROR, "Only Data Set objects are supported in vtkDataObjectToConduit.");
    return false;
  }

  return FillConduitNode(data_set, conduit_node);
}

//----------------------------------------------------------------------------
bool vtkDataObjectToConduit::FillConduitNode(vtkDataSet* data_set, conduit_cpp::Node& conduit_node)
{
  bool is_success = FillTopology(data_set, conduit_node);
  if (is_success)
  {
    is_success = FillFields(data_set, conduit_node);
  }
  return is_success;
}

//----------------------------------------------------------------------------
bool vtkDataObjectToConduit::FillTopology(vtkDataSet* data_set, conduit_cpp::Node& conduit_node)
{
  bool is_success = true;

  if (auto imageData = vtkImageData::SafeDownCast(data_set))
  {
    auto coords_node = conduit_node["coordsets/coords"];

    coords_node["type"] = "uniform";

    int* dimensions = imageData->GetDimensions();
    coords_node["dims/i"] = dimensions[0];
    coords_node["dims/j"] = dimensions[1];
    coords_node["dims/k"] = dimensions[2];

    double* origin = imageData->GetOrigin();
    coords_node["origin/x"] = origin[0];
    coords_node["origin/y"] = origin[1];
    coords_node["origin/z"] = origin[2];

    double* spacing = imageData->GetSpacing();
    coords_node["spacing/dx"] = spacing[0];
    coords_node["spacing/dy"] = spacing[1];
    coords_node["spacing/dz"] = spacing[2];

    auto topologies_node = conduit_node["topologies/mesh"];
    topologies_node["type"] = "uniform";
    topologies_node["coordset"] = "coords";
  }
  else if (auto rectilinear_grid = vtkRectilinearGrid::SafeDownCast(data_set))
  {
    auto coords_node = conduit_node["coordsets/coords"];

    coords_node["type"] = "rectilinear";

    auto x_values_node = coords_node["values/x"];
    is_success = ConvertDataArrayToMCArray(rectilinear_grid->GetXCoordinates(), x_values_node);
    if (is_success)
    {
      auto y_values_node = coords_node["values/y"];
      is_success = ConvertDataArrayToMCArray(rectilinear_grid->GetYCoordinates(), y_values_node);
    }
    if (is_success)
    {
      auto z_values_node = coords_node["values/z"];
      is_success = ConvertDataArrayToMCArray(rectilinear_grid->GetZCoordinates(), z_values_node);
    }

    if (is_success)
    {
      auto topologies_node = conduit_node["topologies/mesh"];
      topologies_node["type"] = "rectilinear";
      topologies_node["coordset"] = "coords";
    }
  }
  else if (auto structured_grid = vtkStructuredGrid::SafeDownCast(data_set))
  {
    auto coords_node = conduit_node["coordsets/coords"];

    coords_node["type"] = "explicit";

    auto x_values_node = coords_node["values/x"];
    auto y_values_node = coords_node["values/y"];
    auto z_values_node = coords_node["values/z"];

    is_success =
      ConvertPoints(structured_grid->GetPoints(), x_values_node, y_values_node, z_values_node);

    if (is_success)
    {
      auto topologies_node = conduit_node["topologies/mesh"];
      topologies_node["type"] = "structured";
      topologies_node["coordset"] = "coords";
      int* dimensions = structured_grid->GetDimensions();
      topologies_node["elements/dims/i"] = dimensions[0];
      topologies_node["elements/dims/j"] = dimensions[1];
      topologies_node["elements/dims/k"] = dimensions[2];
    }
  }
  else if (auto unstructured_grid = vtkUnstructuredGrid::SafeDownCast(data_set))
  {
    if (IsMixedShape(unstructured_grid))
    {
      vtkLogF(ERROR, "Unstructured type with mixed shape type unsupported.");
      is_success = false;
    }

    if (is_success)
    {
      auto coords_node = conduit_node["coordsets/coords"];

      coords_node["type"] = "explicit";

      auto x_values_node = coords_node["values/x"];
      auto y_values_node = coords_node["values/y"];
      auto z_values_node = coords_node["values/z"];

      auto points = unstructured_grid->GetPoints();

      if (!points)
      {
        x_values_node = std::vector<float>();
        y_values_node = std::vector<float>();
        z_values_node = std::vector<float>();
      }
      else
      {
        is_success = ConvertPoints(
          unstructured_grid->GetPoints(), x_values_node, y_values_node, z_values_node);
      }
    }

    if (is_success)
    {
      auto topologies_node = conduit_node["topologies/mesh"];
      topologies_node["type"] = "unstructured";
      topologies_node["coordset"] = "coords";

      int cell_type = VTK_VERTEX;
      auto number_of_cells = unstructured_grid->GetNumberOfCells();
      if (number_of_cells > 0)
      {
        cell_type = unstructured_grid->GetCellType(0);
      }

      switch (cell_type)
      {
        case VTK_HEXAHEDRON:
          topologies_node["elements/shape"] = "hex";
          break;
        case VTK_TETRA:
          topologies_node["elements/shape"] = "tet";
          break;
        case VTK_QUAD:
          topologies_node["elements/shape"] = "quad";
          break;
        case VTK_TRIANGLE:
          topologies_node["elements/shape"] = "tri";
          break;
        case VTK_LINE:
          topologies_node["elements/shape"] = "line";
          break;
        case VTK_VERTEX:
          topologies_node["elements/shape"] = "point";
          break;
        default:
          vtkLog(ERROR, << "Unsupported cell type in unstructured grid. Cell type: " << cell_type);
          break;
      }

      auto cell_connectivity = unstructured_grid->GetCells();
      auto connectivity_node = topologies_node["elements/connectivity"];
      is_success =
        ConvertDataArrayToMCArray(cell_connectivity->GetConnectivityArray(), connectivity_node);
    }
  }
  else
  {
    vtkLogF(ERROR, "Unsupported type.");
    is_success = false;
  }

  return is_success;
}

//----------------------------------------------------------------------------
bool vtkDataObjectToConduit::FillFields(vtkDataSet* data_set, conduit_cpp::Node& conduit_node)
{
  bool is_success = true;

  if (auto cell_data = data_set->GetCellData())
  {
    is_success = FillFields(cell_data, "element", conduit_node);
  }

  if (is_success)
  {
    if (auto point_data = data_set->GetPointData())
    {
      is_success = FillFields(point_data, "vertex", conduit_node);
    }
  }

  if (is_success)
  {
    if (auto field_data = data_set->GetFieldData())
    {
      // field without associated topology is not supported by conduit...
    }
  }

  return is_success;
}

//----------------------------------------------------------------------------
bool vtkDataObjectToConduit::FillFields(
  vtkFieldData* field_data, const std::string& association, conduit_cpp::Node& conduit_node)
{
  bool is_success = true;

  int array_count = field_data->GetNumberOfArrays();
  for (int array_index = 0; is_success && array_index < array_count; ++array_index)
  {
    auto array = field_data->GetArray(array_index);
    auto name = array->GetName();
    if (!name)
    {
      vtkLogF(WARNING, "Unamed array, it will be ignored.");
      continue;
    }

    auto field_node = conduit_node["fields"][array->GetName()];
    field_node["association"] = association;
    field_node["topology"] = "mesh";
    field_node["volume_dependent"] = "false";

    auto values_node = field_node["values"];
    is_success = ConvertDataArrayToMCArray(array, values_node);
  }

  return is_success;
}

//----------------------------------------------------------------------------
bool vtkDataObjectToConduit::ConvertDataArrayToMCArray(
  vtkDataArray* data_array, conduit_cpp::Node& conduit_node)
{
  return ConvertDataArrayToMCArray(data_array, 0, 0, conduit_node);
}

//----------------------------------------------------------------------------
bool vtkDataObjectToConduit::ConvertDataArrayToMCArray(
  vtkDataArray* data_array, int offset, int stride, conduit_cpp::Node& conduit_node)
{
  bool is_success = true;

  stride = std::max(stride, 1);
  conduit_index_t number_of_elements = data_array->GetNumberOfValues() / stride;

  int data_type = data_array->GetDataType();
  int data_type_size = data_array->GetDataTypeSize();
  int array_type = data_array->GetArrayType();

  bool is_supported = true;
  if (IsSignedIntegralType(data_type))
  {
    if (data_type_size == 1)
    {
      if (array_type == vtkAbstractArray::AoSDataArrayTemplate)
      {
        conduit_node.set_external_int8_ptr((conduit_int8*)data_array->GetVoidPointer(0),
          number_of_elements, offset * sizeof(conduit_int8), stride * sizeof(conduit_int8));
      }
      else if (array_type == vtkAbstractArray::SoADataArrayTemplate)
      {
        conduit_node.set_external_int8_ptr((conduit_int8*)data_array->GetVoidPointer(0),
          number_of_elements, offset * sizeof(conduit_int8), stride * sizeof(conduit_int8));
      }
      else
      {
        is_supported = false;
      }
    }
    else if (data_type_size == 2)
    {
      if (array_type == vtkAbstractArray::AoSDataArrayTemplate)
      {
        conduit_node.set_external_int16_ptr((conduit_int16*)data_array->GetVoidPointer(0),
          number_of_elements, offset * sizeof(conduit_int16), stride * sizeof(conduit_int16));
      }
      else if (array_type == vtkAbstractArray::SoADataArrayTemplate)
      {
        conduit_node.set_external_int16_ptr((conduit_int16*)data_array->GetVoidPointer(0),
          number_of_elements, offset * sizeof(conduit_int16), stride * sizeof(conduit_int16));
      }
      else
      {
        is_supported = false;
      }
    }
    else if (data_type_size == 4)
    {
      if (array_type == vtkAbstractArray::AoSDataArrayTemplate)
      {
        conduit_node.set_external_int32_ptr((conduit_int32*)data_array->GetVoidPointer(0),
          number_of_elements, offset * sizeof(conduit_int32), stride * sizeof(conduit_int32));
      }
      else if (array_type == vtkAbstractArray::SoADataArrayTemplate)
      {
        conduit_node.set_external_int32_ptr((conduit_int32*)data_array->GetVoidPointer(0),
          number_of_elements, offset * sizeof(conduit_int32), stride * sizeof(conduit_int32));
      }
      else
      {
        is_supported = false;
      }
    }
    else if (data_type_size == 8)
    {
      if (array_type == vtkAbstractArray::AoSDataArrayTemplate)
      {
        conduit_node.set_external_int64_ptr((conduit_int64*)data_array->GetVoidPointer(0),
          number_of_elements, offset * sizeof(conduit_int64), stride * sizeof(conduit_int64));
      }
      else if (array_type == vtkAbstractArray::SoADataArrayTemplate)
      {
        conduit_node.set_external_int64_ptr((conduit_int64*)data_array->GetVoidPointer(0),
          number_of_elements, offset * sizeof(conduit_int64), stride * sizeof(conduit_int64));
      }
      else
      {
        is_supported = false;
      }
    }
    else
    {
      is_supported = false;
    }
  }
  else if (IsUnsignedIntegralType(data_type))
  {
    if (data_type_size == 1)
    {
      if (array_type == vtkAbstractArray::AoSDataArrayTemplate)
      {
        conduit_node.set_external_uint8_ptr((conduit_uint8*)data_array->GetVoidPointer(0),
          number_of_elements, offset * sizeof(conduit_uint8), stride * sizeof(conduit_uint8));
      }
      else if (array_type == vtkAbstractArray::SoADataArrayTemplate)
      {
        conduit_node.set_external_uint8_ptr((conduit_uint8*)data_array->GetVoidPointer(0),
          number_of_elements, offset * sizeof(conduit_uint8), stride * sizeof(conduit_uint8));
      }
      else
      {
        is_supported = false;
      }
    }
    else if (data_type_size == 2)
    {
      if (array_type == vtkAbstractArray::AoSDataArrayTemplate)
      {
        conduit_node.set_external_uint16_ptr((conduit_uint16*)data_array->GetVoidPointer(0),
          number_of_elements, offset * sizeof(conduit_uint16), stride * sizeof(conduit_uint16));
      }
      else if (array_type == vtkAbstractArray::SoADataArrayTemplate)
      {
        conduit_node.set_external_uint16_ptr((conduit_uint16*)data_array->GetVoidPointer(0),
          number_of_elements, offset * sizeof(conduit_uint16), stride * sizeof(conduit_uint16));
      }
      else
      {
        is_supported = false;
      }
    }
    else if (data_type_size == 4)
    {
      if (array_type == vtkAbstractArray::AoSDataArrayTemplate)
      {
        conduit_node.set_external_uint32_ptr((conduit_uint32*)data_array->GetVoidPointer(0),
          number_of_elements, offset * sizeof(conduit_uint32), stride * sizeof(conduit_uint32));
      }
      else if (array_type == vtkAbstractArray::SoADataArrayTemplate)
      {
        conduit_node.set_external_uint32_ptr((conduit_uint32*)data_array->GetVoidPointer(0),
          number_of_elements, offset * sizeof(conduit_uint32), stride * sizeof(conduit_uint32));
      }
      else
      {
        is_supported = false;
      }
    }
    else if (data_type_size == 8)
    {
      if (array_type == vtkAbstractArray::AoSDataArrayTemplate)
      {
        conduit_node.set_external_uint64_ptr((conduit_uint64*)data_array->GetVoidPointer(0),
          number_of_elements, offset * sizeof(conduit_uint64), stride * sizeof(conduit_uint64));
      }
      else if (array_type == vtkAbstractArray::SoADataArrayTemplate)
      {
        conduit_node.set_external_uint64_ptr((conduit_uint64*)data_array->GetVoidPointer(0),
          number_of_elements, offset * sizeof(conduit_uint64), stride * sizeof(conduit_uint64));
      }
      else
      {
        is_supported = false;
      }
    }
    else
    {
      is_supported = false;
    }
  }
  else if (IsFloatType(data_type))
  {
    if (data_type_size == 4)
    {
      if (array_type == vtkAbstractArray::AoSDataArrayTemplate)
      {
        conduit_node.set_external_float32_ptr((conduit_float32*)data_array->GetVoidPointer(0),
          number_of_elements, offset * sizeof(conduit_float32), stride * sizeof(conduit_float32));
      }
      else if (array_type == vtkAbstractArray::SoADataArrayTemplate)
      {
        conduit_node.set_external_float32_ptr((conduit_float32*)data_array->GetVoidPointer(0),
          number_of_elements, offset * sizeof(conduit_float32), stride * sizeof(conduit_float32));
      }
      else
      {
        is_supported = false;
      }
    }
    else if (data_type_size == 8)
    {
      if (array_type == vtkAbstractArray::AoSDataArrayTemplate)
      {
        conduit_node.set_external_float64_ptr((conduit_float64*)data_array->GetVoidPointer(0),
          number_of_elements, offset * sizeof(conduit_float64), stride * sizeof(conduit_float64));
      }
      else if (array_type == vtkAbstractArray::SoADataArrayTemplate)
      {
        conduit_node.set_external_float64_ptr((conduit_float64*)data_array->GetVoidPointer(0),
          number_of_elements, offset * sizeof(conduit_float64), stride * sizeof(conduit_float64));
      }
      else
      {
        is_supported = false;
      }
    }
    else
    {
      is_supported = false;
    }
  }

  if (!is_supported)
  {
    vtkLog(ERROR, "Unsupported data array type: " << data_array->GetDataTypeAsString() << " size: "
                                                  << data_type_size << " type: " << array_type);
    is_success = false;
  }
  /*
    if (auto soa_data_array = vtkSOADataArrayTemplate<conduit_int8>::SafeDownCast(data_array))
    {
      conduit_node.set_external_int8_ptr(soa_data_array->GetPointer(0), number_of_elements,
        offset * sizeof(conduit_int8), stride * sizeof(conduit_int8));
    }
    else if (auto soa_data_array = vtkSOADataArrayTemplate<conduit_int16>::SafeDownCast(data_array))
    {
      conduit_node.set_external_int16_ptr(soa_data_array->GetPointer(0), number_of_elements,
        offset * sizeof(conduit_int16), stride * sizeof(conduit_int16));
    }
    else if (auto soa_data_array = vtkSOADataArrayTemplate<conduit_int32>::SafeDownCast(data_array))
    {
      conduit_node.set_external_int32_ptr(soa_data_array->GetPointer(0), number_of_elements,
        offset * sizeof(conduit_int32), stride * sizeof(conduit_int32));
    }
    else if (auto soa_data_array = vtkSOADataArrayTemplate<conduit_int64>::SafeDownCast(data_array))
    {
      conduit_node.set_external_int64_ptr(soa_data_array->GetPointer(0), number_of_elements,
        offset * sizeof(conduit_int64), stride * sizeof(conduit_int64));
    }
    else if (auto soa_data_array = vtkSOADataArrayTemplate<conduit_int64>::SafeDownCast(data_array))
    {
      conduit_node.set_external_int64_ptr(soa_data_array->GetPointer(0), number_of_elements,
        offset * sizeof(conduit_int64), stride * sizeof(conduit_int64));
    }
    else if (auto soa_data_array = vtkSOADataArrayTemplate<conduit_uint8>::SafeDownCast(data_array))
    {
      conduit_node.set_external_uint8_ptr(soa_data_array->GetPointer(0), number_of_elements,
        offset * sizeof(conduit_uint8), stride * sizeof(conduit_uint8));
    }
    else if (auto soa_data_array =
    vtkSOADataArrayTemplate<conduit_uint16>::SafeDownCast(data_array))
    {
      conduit_node.set_external_uint16_ptr(soa_data_array->GetPointer(0), number_of_elements, 0,
        soa_data_array->GetNumberOfComponents(), offset * sizeof(conduit_uint16),
        stride * sizeof(conduit_uint16));
    }
    else if (auto soa_data_array =
    vtkSOADataArrayTemplate<conduit_uint32>::SafeDownCast(data_array))
    {
      conduit_node.set_external_uint32_ptr(soa_data_array->GetPointer(0), number_of_elements, 0,
        soa_data_array->GetNumberOfComponents(), offset * sizeof(conduit_uint32),
        stride * sizeof(conduit_uint32));
    }
    else if (auto soa_data_array =
    vtkSOADataArrayTemplate<conduit_uint64>::SafeDownCast(data_array))
    {
      conduit_node.set_external_uint64_ptr(soa_data_array->GetPointer(0), number_of_elements,
        offset * sizeof(conduit_uint64), stride * sizeof(conduit_uint64));
    }
    else if (auto soa_data_array =
    vtkSOADataArrayTemplate<conduit_float32>::SafeDownCast(data_array))
    {
      conduit_node.set_external_float32_ptr(soa_data_array->GetPointer(0), number_of_elements,
        offset * sizeof(conduit_float32), stride * sizeof(conduit_float32));
    }
    else if (auto soa_data_array =
    vtkSOADataArrayTemplate<conduit_float64>::SafeDownCast(data_array))
    {
      conduit_node.set_external_float64_ptr(soa_data_array->GetPointer(0), number_of_elements,
        offset * sizeof(conduit_float64), stride * sizeof(conduit_float64));
    }
    else if (auto aos_data_array = vtkAOSDataArrayTemplate<conduit_int8>::SafeDownCast(data_array))
    {
      conduit_node.set_external_int8_ptr(aos_data_array->GetPointer(0), number_of_elements,
        offset * sizeof(conduit_int8), stride * sizeof(conduit_int8));
    }
    else if (auto aos_data_array = vtkAOSDataArrayTemplate<conduit_int16>::SafeDownCast(data_array))
    {
      conduit_node.set_external_int16_ptr(aos_data_array->GetPointer(0), number_of_elements,
        offset * sizeof(conduit_int16), stride * sizeof(conduit_int16));
    }
    else if (auto aos_data_array = vtkAOSDataArrayTemplate<conduit_int32>::SafeDownCast(data_array))
    {
      conduit_node.set_external_int32_ptr(aos_data_array->GetPointer(0), number_of_elements,
        offset * sizeof(conduit_int32), stride * sizeof(conduit_int32));
    }
    else if (auto aos_data_array = vtkAOSDataArrayTemplate<conduit_int64>::SafeDownCast(data_array))
    {
      conduit_node.set_external_int64_ptr(aos_data_array->GetPointer(0), number_of_elements, 0,
        aos_data_array->GetNumberOfComponents(), offset * sizeof(conduit_int64),
        stride * sizeof(conduit_int64));
    }
    else if (auto aos_data_array = vtkAOSDataArrayTemplate<conduit_uint8>::SafeDownCast(data_array))
    {
      conduit_node.set_external_uint8_ptr(aos_data_array->GetPointer(0), number_of_elements,
        offset * sizeof(conduit_uint8), stride * sizeof(conduit_uint8));
    }
    else if (auto aos_data_array =
    vtkAOSDataArrayTemplate<conduit_uint16>::SafeDownCast(data_array))
    {
      conduit_node.set_external_uint16_ptr(aos_data_array->GetPointer(0), number_of_elements,
        offset * sizeof(conduit_uint16), stride * sizeof(conduit_uint16));
    }
    else if (auto aos_data_array =
    vtkAOSDataArrayTemplate<conduit_uint32>::SafeDownCast(data_array))
    {
      conduit_node.set_external_uint32_ptr(aos_data_array->GetPointer(0), number_of_elements,
        offset * sizeof(conduit_uint32), stride * sizeof(conduit_uint32));
    }
    else if (auto aos_data_array =
    vtkAOSDataArrayTemplate<conduit_uint64>::SafeDownCast(data_array))
    {
      conduit_node.set_external_uint64_ptr(aos_data_array->GetPointer(0), number_of_elements, 0,
        aos_data_array->GetNumberOfComponents(), offset * sizeof(conduit_uint64),
        stride * sizeof(conduit_uint64));
    }
    else if (auto aos_data_array =
    vtkAOSDataArrayTemplate<conduit_float32>::SafeDownCast(data_array))
    {
      conduit_node.set_external_float32_ptr(aos_data_array->GetPointer(0), number_of_elements,
        offset * sizeof(conduit_float32), stride * sizeof(conduit_float32));
    }
    else if (auto aos_data_array =
    vtkAOSDataArrayTemplate<conduit_float64>::SafeDownCast(data_array))
    {
      conduit_node.set_external_float64_ptr(aos_data_array->GetPointer(0), number_of_elements,
        offset * sizeof(conduit_float64), stride * sizeof(conduit_float64));
    }
    else
    {
      vtkLog(ERROR, "Unsupported data array type: " << data_array->GetDataTypeAsString());
      is_success = false;
    }
  */
  return is_success;
}

//----------------------------------------------------------------------------
bool vtkDataObjectToConduit::ConvertPoints(vtkPoints* points, conduit_cpp::Node& x_values_node,
  conduit_cpp::Node& y_values_node, conduit_cpp::Node& z_values_node)
{
  bool is_success = true;

  auto data_array = points->GetData();
  is_success = data_array;

  if (is_success)
  {
    is_success = ConvertDataArrayToMCArray(data_array, 0, 3, x_values_node);
  }

  if (is_success)
  {
    is_success = ConvertDataArrayToMCArray(data_array, 1, 3, y_values_node);
  }

  if (is_success)
  {
    is_success = ConvertDataArrayToMCArray(data_array, 2, 3, z_values_node);
  }

  return is_success;
}

//----------------------------------------------------------------------------
bool vtkDataObjectToConduit::IsMixedShape(vtkUnstructuredGrid* unstructured_grid)
{
  vtkNew<vtkCellTypes> cell_types;
  unstructured_grid->GetCellTypes(cell_types);
  return cell_types->GetNumberOfTypes() > 1;
}

//----------------------------------------------------------------------------
bool vtkDataObjectToConduit::IsSignedIntegralType(int data_type)
{
#if (CHAR_MIN == SCHAR_MIN && CHAR_MAX == SCHAR_MAX)
  // the char type is signed on this compiler
  return ((data_type == VTK_CHAR) || (data_type == VTK_SIGNED_CHAR) || (data_type == VTK_SHORT) ||
    (data_type == VTK_INT) || (data_type == VTK_LONG) || (data_type == VTK_ID_TYPE) ||
    (data_type == VTK_LONG_LONG) || (data_type == VTK_TYPE_INT64));
#else
  // char is unsigned
  return ((data_type == VTK_SIGNED_CHAR) || (data_type == VTK_SHORT) || (data_type == VTK_INT) ||
    (data_type == VTK_LONG) || (data_type == VTK_ID_TYPE) || (data_type == VTK_LONG_LONG) ||
    (data_type == VTK_TYPE_INT64));
#endif
}

//----------------------------------------------------------------------------
bool vtkDataObjectToConduit::IsUnsignedIntegralType(int data_type)
{
#if (CHAR_MIN == SCHAR_MIN && CHAR_MAX == SCHAR_MAX)
  // the char type is signed on this compiler
  return ((data_type == VTK_UNSIGNED_CHAR) || (data_type == VTK_UNSIGNED_SHORT) ||
    (data_type == VTK_UNSIGNED_INT) || (data_type == VTK_UNSIGNED_LONG) ||
    (data_type == VTK_ID_TYPE) || (data_type == VTK_UNSIGNED_LONG_LONG));
#else
  // char is unsigned
  return ((data_type == VTK_CHAR) || (data_type == VTK_UNSIGNED_CHAR) || (data_type == VTK_SHORT) ||
    (data_type == VTK_INT) || (data_type == VTK_LONG) || (data_type == VTK_ID_TYPE) ||
    (data_type == VTK_LONG_LONG) || (data_type == VTK_TYPE_INT64));
#endif
}

//----------------------------------------------------------------------------
bool vtkDataObjectToConduit::IsFloatType(int data_type)
{
  return ((data_type == VTK_FLOAT) || (data_type == VTK_DOUBLE));
}

//----------------------------------------------------------------------------
void vtkDataObjectToConduit::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os, indent);
}
