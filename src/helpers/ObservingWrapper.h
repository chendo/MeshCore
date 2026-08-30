#pragma once

#include "radiolib/CustomSX1262Wrapper.h"
#include "MeshObserver.h"

/**
 * \brief  A radio wrapper that also feeds a MeshObserver.
 *
 * MeshObserver builds its peer table from RAW frames, and until now the only
 * thing that fed it was SharedRadioCore -- which exists on hydra and nowhere
 * else. A single-identity node (the companion) therefore had no way to know
 * who it can hear, even though every frame passes through its radio.
 *
 * This taps the one place those frames already surface. It is selected with the
 * established build-flag idiom rather than by editing anything:
 *
 *   build_unflags = -D WRAPPER_CLASS=CustomSX1262Wrapper
 *   build_flags     = -D WRAPPER_CLASS=ObservingSX1262Wrapper
 *   build_src_flags = -include src/helpers/ObservingWrapper.h
 *
 * The force-include is needed because target.h names WRAPPER_CLASS before
 * anything of ours could have been included. It must go in build_src_FLAGS,
 * not build_flags: the latter reaches library sources too, which do not carry
 * -I src and fail on the first line of this file.
 *
 * NOTE: PlatformIO does not rebuild when a force-included header changes -- run
 * `-t clean` after editing this file, or you will flash the previous build and
 * debug a ghost.
 */

/** The node's observer. One per node, since there is one radio. */
MeshObserver& rfObserver();

class ObservingSX1262Wrapper : public CustomSX1262Wrapper {
public:
  ObservingSX1262Wrapper(CustomSX1262& radio, mesh::MainBoard& board)
    : CustomSX1262Wrapper(radio, board) { }

  int recvRaw(uint8_t* bytes, int sz) override {
    int len = CustomSX1262Wrapper::recvRaw(bytes, sz);
    if (len > 0) {
      /* SNR in quarter-dB, the same scale the packet log uses. Read straight
         after the frame, while the modem still describes THAT frame. */
      rfObserver().observeRx(bytes, len, (int8_t)(getLastSNR() * 4));
    }
    return len;
  }
};
