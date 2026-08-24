#include "ObservingWrapper.h"

/* Function-local static, not a file-scope object: the observer is reached from
   a radio wrapper that a variant constructs at static-init time, and the order
   of static initialisation across translation units is not defined. This way it
   is built on first use, which is necessarily after main() starts. */
MeshObserver& rfObserver() {
  static MeshObserver obs;
  return obs;
}
