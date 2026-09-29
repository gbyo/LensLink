/*
 * Tiny embedded HTTP server providing a browser control panel for the
 * connected camera devices (zoom / exposure / focus / flashlight),
 * DroidCam-style. Serves on 127.0.0.1 only; control commands are queued
 * onto the addressed source and forwarded to the device over the stream
 * connection.
 *
 * One server, many sources. Sources come from the shared source registry
 * (source-registry.h) — the same one the Qt dock and diagnostics read —
 * and requests address a camera source with the ?src= id listed by
 * /api/sources; no id means the first camera source, so single-source
 * setups and older scripts work unchanged.
 */

#pragma once

/* Reconciles the server with the plugin-wide enable/port settings and
 * the registry: it runs while the setting is on and at least one camera
 * source exists. Call after a camera source is added or removed and after
 * the plugin-wide settings change. Not from a request handler. */
void web_control_apply_settings(void);
void web_control_shutdown(void);
