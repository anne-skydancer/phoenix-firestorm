//! P0 (GROUNDUP_VULKAN_ENGINE_PLAN.md): the typed scene-bridge scaffold.
//!
//! This is the input the viewer will eventually fill with typed scene + settings DATA,
//! replacing the GL-draw tap (FSSceneDump). It stands up BESIDE the tap: through P0-P11 the
//! tap still renders whatever has not migrated, and this typed frame contributes nothing
//! until a migration phase fills it. The screen is never blank.
//!
//! P0 scope: the frame lifecycle + the first POD contract struct (`CameraBlock`, consumed in
//! P1). No rendering yet -- proven inert by the unit tests here + the unchanged headless
//! render tests. Each later phase adds one payload (settings snapshot, EEP env, light array,
//! batches, ...) and the seam where the engine renders it.

/// The camera for a typed frame (P1 payload). `#[repr(C)]`, to be mirrored in C++ when the
/// viewer wiring lands. Column-major mat4s (glam/GL order). The engine derives its OWN
/// reverse-Z projection from `near`/`far` rather than consuming a per-draw baked MVP.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct CameraBlock {
    pub view: [f32; 16],
    pub proj: [f32; 16],
    pub origin: [f32; 3],
    pub near: f32,
    pub far: f32,
    pub fov_y: f32,
    pub aspect: f32,
    pub viewport_w: f32,
    pub viewport_h: f32,
}

/// The accumulating typed frame. Grows one payload per phase; P0 carries only the lifecycle
/// and the camera slot, and is empty by default so it renders nothing.
#[derive(Default)]
pub struct SceneFrame {
    /// Typed frames begun (telemetry / co-existence proof).
    pub frames: u64,
    /// P1: the frame camera. `None` until the viewer feeds it.
    pub camera: Option<CameraBlock>,
}

impl SceneFrame {
    pub fn new() -> SceneFrame {
        SceneFrame::default()
    }

    /// Start a typed frame: clear the previous frame's accumulated data. Called from
    /// `fsr_scene_begin` at the top of the frame, alongside `fsr_begin_frame` (the tap).
    pub fn begin(&mut self) {
        self.frames = self.frames.wrapping_add(1);
        self.camera = None;
    }

    pub fn set_camera(&mut self, cam: &CameraBlock) {
        self.camera = Some(*cam);
    }

    /// Whether this typed frame carries anything to render. P0: only a camera can be present,
    /// which alone produces no draws -- so the engine still renders the tap unchanged. This is
    /// the seam a migration phase flips once it contributes real typed geometry.
    pub fn is_empty(&self) -> bool {
        self.camera.is_none()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn lifecycle_accumulates_and_resets() {
        let mut s = SceneFrame::new();
        assert!(s.is_empty(), "fresh frame is empty");
        s.begin();
        assert_eq!(s.frames, 1);
        let cam = CameraBlock { near: 0.1, far: 4096.0, fov_y: 1.0, aspect: 1.5, ..Default::default() };
        s.set_camera(&cam);
        assert!(!s.is_empty());
        assert_eq!(s.camera.unwrap().far, 4096.0);
        // begin() resets the previous frame's payload.
        s.begin();
        assert_eq!(s.frames, 2);
        assert!(s.is_empty(), "begin cleared the camera");
    }
}
