package org.basaltmc.proxy

public object BuildInfo {
    public const val VERSION: String = "0.1.0-SNAPSHOT"

    /** The protocol the proxy speaks natively. Older clients are translated, see basalt-multiversion. */
    public const val PROTOCOL_VERSION: Int = 776

    public const val MINECRAFT_VERSION: String = "26.2"
}
