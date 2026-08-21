// P0e3 custom blend test asset.
// Drop this script into a rezzed prim. Touch the prim to toggle the emitter.

integer ParticlesEnabled = TRUE;

start_particles()
{
    llParticleSystem([
        PSYS_SRC_PATTERN, PSYS_SRC_PATTERN_EXPLODE,
        PSYS_SRC_BURST_RADIUS, 0.15,
        PSYS_SRC_BURST_RATE, 0.10,
        PSYS_SRC_BURST_PART_COUNT, 8,
        PSYS_SRC_BURST_SPEED_MIN, 0.05,
        PSYS_SRC_BURST_SPEED_MAX, 0.20,
        PSYS_SRC_ACCEL, <0.0, 0.0, 0.08>,
        PSYS_SRC_MAX_AGE, 0.0,

        PSYS_PART_MAX_AGE, 2.5,
        PSYS_PART_START_COLOR, <1.0, 0.15, 0.02>,
        PSYS_PART_END_COLOR, <0.02, 0.55, 1.0>,
        PSYS_PART_START_ALPHA, 0.75,
        PSYS_PART_END_ALPHA, 0.0,
        PSYS_PART_START_SCALE, <0.35, 0.35, 0.0>,
        PSYS_PART_END_SCALE, <0.08, 0.08, 0.0>,
        PSYS_PART_START_GLOW, 0.08,
        PSYS_PART_END_GLOW, 0.0,

        // Additive custom blend. Standard alpha would use
        // SOURCE_ALPHA + ONE_MINUS_SOURCE_ALPHA.
        PSYS_PART_BLEND_FUNC_SOURCE, PSYS_PART_BF_SOURCE_ALPHA,
        PSYS_PART_BLEND_FUNC_DEST, PSYS_PART_BF_ONE,

        PSYS_PART_FLAGS,
            PSYS_PART_INTERP_COLOR_MASK |
            PSYS_PART_INTERP_SCALE_MASK |
            PSYS_PART_EMISSIVE_MASK
    ]);

    llSetText("CUSTOM BLEND: SOURCE_ALPHA + ONE\nTouch to stop",
        <1.0, 0.45, 0.10>, 1.0);
}

stop_particles()
{
    llParticleSystem([]);
    llSetText("Custom blend stopped\nTouch to start", <0.7, 0.7, 0.7>, 1.0);
}

default
{
    state_entry()
    {
        ParticlesEnabled = TRUE;
        start_particles();
    }

    touch_start(integer detected_count)
    {
        if (ParticlesEnabled)
        {
            ParticlesEnabled = FALSE;
            stop_particles();
        }
        else
        {
            ParticlesEnabled = TRUE;
            start_particles();
        }
    }

    on_rez(integer start_parameter)
    {
        llResetScript();
    }

    changed(integer change_flags)
    {
        if (change_flags & CHANGED_OWNER)
        {
            llResetScript();
        }
    }
}