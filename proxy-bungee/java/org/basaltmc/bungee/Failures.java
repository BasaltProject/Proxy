// Independent reimplementation of the BungeeCord API shape over Basalt natives. See proxy-bungee/NOTICE.md
package org.basaltmc.bungee;

/** Turns a plugin failure into log text with its causes and the frames that locate it. */
final class Failures {
    private static final int kFrames = 6;

    private Failures() {}

    static String describe(Throwable error) {
        final StringBuilder text = new StringBuilder();
        for (Throwable cause = error; cause != null; cause = cause.getCause()) {
            text.append(cause == error ? "" : "\n  caused by ").append(cause);
            final StackTraceElement[] frames = cause.getStackTrace();
            for (int i = 0; i < Math.min(kFrames, frames.length); i++) {
                text.append("\n    at ").append(frames[i]);
            }
            if (cause.getCause() == cause) {
                break;
            }
        }
        return text.toString();
    }

    static void log(String what, Throwable error) {
        Natives.log(what + ": " + describe(error));
    }
}
