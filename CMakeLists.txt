cmake_minimum_required(VERSION 3.24)
project(Graph3D LANGUAGES CXX)

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets)
if(Qt6_VERSION VERSION_GREATER_EQUAL 7)
    message(FATAL_ERROR "Graph3D requires Qt 6")
endif()
find_package(OpenCASCADE REQUIRED COMPONENTS
    FoundationClasses ModelingData ModelingAlgorithms Visualization ApplicationFramework DataExchange)
if(NOT OpenCASCADE_MAJOR_VERSION EQUAL 7 OR OpenCASCADE_MINOR_VERSION LESS 9)
    message(FATAL_ERROR "Graph3D requires Open CASCADE Technology 7.9 or newer within 7.x")
endif()

add_library(graph3d_geometry STATIC
    src/geometry/PreviewBox.cpp
    src/geometry/Primitives.cpp
    src/geometry/SketchModeling.cpp
    src/geometry/SketchConstraints.cpp
    src/geometry/PartFeatures.cpp
    src/geometry/AdvancedModeling.cpp
    src/geometry/BodyTransform.cpp
    src/geometry/Assembly.cpp src/geometry/AssemblyMates.cpp)
target_compile_features(graph3d_geometry PUBLIC cxx_std_20)
set_target_properties(graph3d_geometry PROPERTIES CXX_EXTENSIONS OFF)
if(WIN32)
    # OCCT 7 headers use M_PI and related constants from the Windows CRT.
    target_compile_definitions(graph3d_geometry PUBLIC _USE_MATH_DEFINES)
endif()
target_include_directories(graph3d_geometry PUBLIC src)
target_include_directories(graph3d_geometry SYSTEM PUBLIC "${OpenCASCADE_INCLUDE_DIR}")
target_link_libraries(graph3d_geometry PUBLIC TKBRep TKMath TKernel PRIVATE TKPrim TKTopAlgo TKBO TKFillet TKShHealing TKG3d TKOffset)

add_library(graph3d_document STATIC src/document/Document.cpp)
set_target_properties(graph3d_document PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(graph3d_document PUBLIC graph3d_geometry)

add_library(graph3d_drawing STATIC src/drawing/Projection.cpp src/drawing/ProjectedEdges.cpp src/drawing/ModelReference.cpp src/drawing/ModelViews.cpp src/drawing/DrawingSheet.cpp src/drawing/Draft.cpp src/drawing/DraftCurves.cpp src/drawing/Specification.cpp)
set_target_properties(graph3d_drawing PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(graph3d_drawing PUBLIC graph3d_geometry Qt6::Gui PRIVATE TKHLR TKG3d TKTopAlgo TKGeomBase TKPrim TKBO)

add_library(graph3d_io STATIC src/io/FileIO.cpp src/io/ProjectIO.cpp src/io/LinkedComponents.cpp src/io/Exchange.cpp src/io/DraftIO.cpp src/io/DxfIO.cpp src/io/ModelViewsIO.cpp src/io/AssemblyDrawingIO.cpp)
set_target_properties(graph3d_io PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(graph3d_io PUBLIC graph3d_document Qt6::Core
    PRIVATE graph3d_drawing TKDESTEP TKXSBase TKMesh TKTopAlgo)

add_executable(Graph3D WIN32 src/app/main.cpp)
set_target_properties(Graph3D PROPERTIES CXX_EXTENSIONS OFF)
qt_add_resources(Graph3D app_icon PREFIX "/" FILES resources/icons/graph3d.png)
if(WIN32)
    enable_language(RC)
    set(GRAPH3D_ICON "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/graph3d.ico")
    configure_file(resources/Graph3D.rc.in Graph3D.rc @ONLY)
    set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/Graph3D.rc"
        PROPERTIES OBJECT_DEPENDS "${GRAPH3D_ICON}")
    target_sources(Graph3D PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/Graph3D.rc")
endif()
add_library(graph3d_ui STATIC src/gui/DocumentWindow.cpp src/gui/DocumentWindow.h src/gui/MainWindow.cpp src/gui/SketchEditor.cpp
    src/gui/SketchEditor.h src/gui/PropertyPanel.cpp src/gui/PropertyPanel.h src/gui/DrawingDialog.cpp
    src/gui/TransformEditor.cpp src/gui/AdvancedDialog.cpp
    src/gui/AssemblyDialog.cpp
    src/gui/StartPage.cpp src/gui/StartPage.h
    src/gui/UserPreferences.cpp src/gui/InterfaceLanguage.cpp
    src/gui/LineInput.cpp src/gui/LineInput.h
    src/gui/DraftCanvas.cpp src/gui/DraftCanvas.h src/gui/DraftWindow.cpp src/gui/DraftWindow.h src/gui/DraftAssembly.cpp src/gui/DraftViews.cpp src/gui/DraftTools.cpp
    src/viewport/Viewport.cpp
    src/viewport/Viewport.h)
qt_add_resources(graph3d_ui start_examples PREFIX "/" FILES
    examples/flange.graph3d examples/stepped-bushing.graph3d examples/angle-bracket.graph3d)
qt_add_resources(graph3d_ui interface_language PREFIX "/" FILES resources/translations/ru.json)
set_target_properties(graph3d_ui PROPERTIES AUTOMOC ON CXX_EXTENSIONS OFF)
target_link_libraries(graph3d_ui PUBLIC Qt6::Widgets graph3d_document
    PRIVATE graph3d_io graph3d_drawing TKV3d TKOpenGl TKService)
target_link_libraries(Graph3D PRIVATE graph3d_ui)

include(CTest)
if(BUILD_TESTING)
    add_executable(preview_box_test tests/geometry/PreviewBoxTest.cpp)
    set_target_properties(preview_box_test PROPERTIES CXX_EXTENSIONS OFF)
    target_link_libraries(preview_box_test PRIVATE graph3d_geometry)
    add_test(NAME preview_box COMMAND preview_box_test)

    add_executable(primitives_test tests/geometry/PrimitivesTest.cpp)
    set_target_properties(primitives_test PROPERTIES CXX_EXTENSIONS OFF)
    target_link_libraries(primitives_test PRIVATE graph3d_geometry TKTopAlgo)
    add_test(NAME primitives COMMAND primitives_test)

    add_executable(sketch_modeling_test tests/geometry/SketchModelingTest.cpp)
    set_target_properties(sketch_modeling_test PROPERTIES CXX_EXTENSIONS OFF)
    target_link_libraries(sketch_modeling_test PRIVATE graph3d_geometry TKTopAlgo)
    add_test(NAME sketch_modeling COMMAND sketch_modeling_test)

    add_executable(document_test tests/document/DocumentTest.cpp)
    set_target_properties(document_test PROPERTIES CXX_EXTENSIONS OFF)
    target_link_libraries(document_test PRIVATE graph3d_document TKTopAlgo)
    add_test(NAME document COMMAND document_test)

    add_executable(exchange_test tests/io/ExchangeTest.cpp)
    set_target_properties(exchange_test PROPERTIES CXX_EXTENSIONS OFF)
    target_link_libraries(exchange_test PRIVATE graph3d_io TKTopAlgo)
    add_test(NAME exchange COMMAND exchange_test)

    add_executable(part_features_test tests/geometry/PartFeaturesTest.cpp)
    set_target_properties(part_features_test PROPERTIES CXX_EXTENSIONS OFF)
    target_link_libraries(part_features_test PRIVATE graph3d_io TKTopAlgo)
    add_test(NAME part_features COMMAND part_features_test)

    add_executable(body_transform_test tests/geometry/BodyTransformTest.cpp)
    set_target_properties(body_transform_test PROPERTIES CXX_EXTENSIONS OFF)
    target_link_libraries(body_transform_test PRIVATE graph3d_io TKTopAlgo)
    add_test(NAME body_transform COMMAND body_transform_test)

    add_executable(assembly_test tests/document/AssemblyTest.cpp)
    set_target_properties(assembly_test PROPERTIES CXX_EXTENSIONS OFF)
    target_link_libraries(assembly_test PRIVATE graph3d_io TKTopAlgo)
    add_test(NAME assembly COMMAND assembly_test)

    add_executable(drawing_test tests/drawing/DrawingTest.cpp)
    set_target_properties(drawing_test PROPERTIES CXX_EXTENSIONS OFF)
    target_link_libraries(drawing_test PRIVATE graph3d_drawing graph3d_io TKTopAlgo)
    add_test(NAME drawing COMMAND drawing_test "${CMAKE_CURRENT_SOURCE_DIR}/examples")
    set_tests_properties(drawing PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    option(GRAPH3D_GUI_TESTS "Run GUI tests on an interactive OpenGL desktop" OFF)
    if(GRAPH3D_GUI_TESTS)
        find_package(Qt6 REQUIRED COMPONENTS Test)
        add_executable(viewport_test tests/viewport/ViewportTest.cpp)
        set_target_properties(viewport_test PROPERTIES AUTOMOC ON CXX_EXTENSIONS OFF)
        target_link_libraries(viewport_test PRIVATE graph3d_ui Qt6::Test)
        add_test(NAME viewport COMMAND viewport_test)
        set_tests_properties(viewport PROPERTIES TIMEOUT 75)
    endif()
endif()
