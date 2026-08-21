#pragma once

// RadioLibWrappers.h includes <Mesh.h> only to reach mesh::Radio and
// mesh::MainBoard, which are declared one layer down. Pull in the real headers
// rather than restating the interfaces here, so a signature change upstream
// breaks the build instead of silently drifting from what is being tested.
#include <Dispatcher.h>
