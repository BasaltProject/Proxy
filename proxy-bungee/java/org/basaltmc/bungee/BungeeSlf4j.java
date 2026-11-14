// Independent reimplementation of the BungeeCord API shape over Basalt natives. See proxy-bungee/NOTICE.md
package org.basaltmc.bungee;

import java.util.logging.Level;
import java.util.logging.Logger;
import org.slf4j.ILoggerFactory;
import org.slf4j.IMarkerFactory;
import org.slf4j.Marker;
import org.slf4j.helpers.AbstractLogger;
import org.slf4j.helpers.BasicMarkerFactory;
import org.slf4j.helpers.MessageFormatter;
import org.slf4j.helpers.NOPMDCAdapter;
import org.slf4j.spi.MDCAdapter;
import org.slf4j.spi.SLF4JServiceProvider;

/** Routes slf4j to java.util.logging, as BungeeCord does, so plugins that log through LoggerFactory are heard. */
public final class BungeeSlf4j implements SLF4JServiceProvider {
    private static final class JulLogger extends AbstractLogger {
        private static final long serialVersionUID = 1L;
        private final transient Logger target;

        JulLogger(String name) {
            this.name = name;
            this.target = Logger.getLogger(name);
        }

        @Override
        protected String getFullyQualifiedCallerName() {
            return null;
        }

        @Override
        protected void handleNormalizedLoggingCall(org.slf4j.event.Level level, Marker marker, String message, Object[] args,
                Throwable error) {
            final Level jul = switch (level) {
                case ERROR -> Level.SEVERE;
                case WARN -> Level.WARNING;
                case INFO -> Level.INFO;
                case DEBUG -> Level.FINE;
                case TRACE -> Level.FINER;
            };
            target.log(jul, "[" + name + "] " + MessageFormatter.arrayFormat(message, args).getMessage(), error);
        }

        private boolean on(Level level) {
            return target.isLoggable(level);
        }

        @Override
        public boolean isTraceEnabled() {
            return on(Level.FINER);
        }

        @Override
        public boolean isTraceEnabled(Marker marker) {
            return on(Level.FINER);
        }

        @Override
        public boolean isDebugEnabled() {
            return on(Level.FINE);
        }

        @Override
        public boolean isDebugEnabled(Marker marker) {
            return on(Level.FINE);
        }

        @Override
        public boolean isInfoEnabled() {
            return on(Level.INFO);
        }

        @Override
        public boolean isInfoEnabled(Marker marker) {
            return on(Level.INFO);
        }

        @Override
        public boolean isWarnEnabled() {
            return on(Level.WARNING);
        }

        @Override
        public boolean isWarnEnabled(Marker marker) {
            return on(Level.WARNING);
        }

        @Override
        public boolean isErrorEnabled() {
            return on(Level.SEVERE);
        }

        @Override
        public boolean isErrorEnabled(Marker marker) {
            return on(Level.SEVERE);
        }
    }

    private final IMarkerFactory markers = new BasicMarkerFactory();
    private final MDCAdapter mdc = new NOPMDCAdapter();
    private final ILoggerFactory loggers = JulLogger::new;

    @Override
    public ILoggerFactory getLoggerFactory() {
        return loggers;
    }

    @Override
    public IMarkerFactory getMarkerFactory() {
        return markers;
    }

    @Override
    public MDCAdapter getMDCAdapter() {
        return mdc;
    }

    @Override
    public String getRequestedApiVersion() {
        return "2.0.99";
    }

    @Override
    public void initialize() {}
}
