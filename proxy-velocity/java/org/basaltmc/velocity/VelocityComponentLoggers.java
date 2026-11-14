// Independent reimplementation of the Velocity API shape over Basalt natives. See proxy-velocity/NOTICE.md
package org.basaltmc.velocity;

import net.kyori.adventure.text.logger.slf4j.ComponentLogger;
import net.kyori.adventure.text.logger.slf4j.ComponentLoggerProvider;

/** Hands out component loggers that write to the proxy log. */
public final class VelocityComponentLoggers implements ComponentLoggerProvider {
    @Override
    public ComponentLogger logger(LoggerHelper helper, String name) {
        return helper.delegating(new VelocityLogger(name), helper.plainSerializer());
    }
}
