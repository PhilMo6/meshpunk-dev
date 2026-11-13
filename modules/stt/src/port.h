// port.h — the engine's line to the audio plugin host (plugin.cpp): the log
// line, the model file and the internal-SRAM tile.

#ifndef PORT_H_
#define PORT_H_

#include <cstddef>
#include <cstdint>

// One line on the firmware console, prefixed with the plugin's name.
extern "C" void stt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Read len bytes at offset of the model file into dst. A dst inside the tile
// is read into directly (len must fit the tile from there); any other dst
// gets the bytes through the tile in kTileBytes pieces, so it must not
// overlap the tile. false on a short read or an I/O error (logged).
bool stt_model_read(uint32_t offset, void *dst, uint32_t len);

// The internal-SRAM tile (config.h kTileBytes) and the bias rows
// (kBiasTileBytes) the kernels read their B operand and bias from.
uint8_t *stt_tile(void);
int32_t *stt_bias_tile(void);

// Called by the tiled kernels (tlib_linear_impl) after each block of B rows:
// the plugin takes the microphone's audio and the app's commands here.
void stt_tile_done(void);

// Called by conformer::Forward after each of its 16 layers. true = end the
// pass now (Forward returns an empty tensor).
bool stt_layer_done(uint16_t layer);

#endif
