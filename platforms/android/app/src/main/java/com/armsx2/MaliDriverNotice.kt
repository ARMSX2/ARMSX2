package com.armsx2

/**
 * The "get malisx2" notice: a Mali GPU that the driver list offers malisx2 for is running the
 * Vulkan renderer on some other driver, whether Arm's own or another pack.
 *
 * Two facts decide it, and each comes from where it is known:
 *  - the driver in use comes from native (NativeApp.getActiveVulkanDriver), read off the open
 *    Vulkan device, so a pack that failed to load counts as the system driver it fell back to.
 *    The software renderer can sit on a Vulkan device too, only to present the frame it drew on
 *    the CPU, so the notice also wants NativeApp.isHardwareRenderer();
 *  - whether the GPU is one malisx2 is offered for is [CustomDriver.offersMaliSX2], the predicate
 *    the driver manager lists the pack by, so the notice and the download appear together.
 *
 * Pure, so it can be unit-tested. The wiring that asks native and shows the notice is in
 * MainActivityRuntime.checkMaliDriver.
 */
object MaliDriverNotice {
    // GSDriverReport::ActiveVulkanDriver in pcsx2/GS/DriverReport/GSDriverReportActive.h.
    const val DRIVER_NONE = 0
    const val DRIVER_MALISX2 = 1
    const val DRIVER_OTHER = 2

    /** The decision: show the notice when the Vulkan renderer is up on a GPU that is offered
     *  malisx2, and the driver in use is not malisx2. */
    internal fun shouldWarn(
        vulkanActive: Boolean,
        gpuOffersMaliSX2: Boolean,
        driverIsMaliSX2: Boolean,
    ): Boolean = vulkanActive && gpuOffersMaliSX2 && !driverIsMaliSX2

    /** [shouldWarn] from what native and the GL probe report: [activeVulkanDriver] is one of the
     *  DRIVER_* values, [hardwareRenderer] whether the GS is drawing on the GPU rather than the
     *  CPU, [glRenderer] the system GL_RENDERER (null when it could not be read). */
    internal fun shouldWarn(activeVulkanDriver: Int, hardwareRenderer: Boolean, glRenderer: String?): Boolean =
        shouldWarn(
            vulkanActive = hardwareRenderer &&
                (activeVulkanDriver == DRIVER_MALISX2 || activeVulkanDriver == DRIVER_OTHER),
            gpuOffersMaliSX2 = CustomDriver.offersMaliSX2(glRenderer),
            driverIsMaliSX2 = activeVulkanDriver == DRIVER_MALISX2,
        )
}
