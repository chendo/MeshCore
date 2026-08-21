#pragma once

// RadioLibWrappers.h includes <Mesh.h> only to get mesh::Radio and
// mesh::MainBoard. Headers one level lower declare both of them. This mock
// includes the real headers. It does not write the interfaces again. Thus a
// change to a signature upstream stops the build. The test cannot move away
// from the real code without a warning.
#include <Dispatcher.h>
