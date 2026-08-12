package org.basaltmc.proxy.runtime

public object BuildInfo {
    public const val VERSION: String = "0.1.0-SNAPSHOT"

    /** The protocol the proxy speaks natively. Older clients are translated, see wire-multiversion. */
    public const val PROTOCOL_VERSION: Int = 776

    public const val MINECRAFT_VERSION: String = "26.2"
}
