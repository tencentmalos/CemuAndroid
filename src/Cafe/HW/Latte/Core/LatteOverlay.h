#pragma once

void LatteOverlay_init();
void LatteOverlay_render(bool pad_view);
void LatteOverlay_renderStatusLayer(bool pad_view);
void LatteOverlay_getStatusLayerCanvasSize(bool pad_view, sint32& width, sint32& height);
void LatteOverlay_updateStats(double fps, sint32 drawcalls, sint32 fastDrawcalls);

void LatteOverlay_pushNotification(const std::string& text, sint32 duration);
