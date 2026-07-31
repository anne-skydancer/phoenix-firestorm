//! Live draw-stream renderer (PHASE3_PLAN.md P3c) — the proven replay renderer
//! (vkharness/src/replay.rs, gate-1 passed on a real sim frame) reshaped for live
//! per-frame submission from the viewer's LLVertexBuffer taps.
//!
//! Contract per frame: fsr_begin_frame() -> N x fsr_submit(desc, vtx, idx) -> fsr_end_frame().
//! Vertex bytes are the viewer's SoA blocks (calcOffsets: per-attribute contiguous,
//! 16-byte aligned, TYPE_VERTEX = vec4 stride 16 w/ .w = texture index) — bound at three
//! slots (pos/uv/color) with per-slot offsets, exactly as the replay proved.

use std::collections::HashMap;

use glam::Mat4;
use wgpu::util::DeviceExt;

// LLVertexBuffer type sizes (llvertexbuffer.cpp:714) — SoA block strides
const TYPE_SIZES: [u32; 14] = [16, 16, 8, 8, 8, 8, 4, 4, 16, 4, 16, 16, 8, 16];
const TYPE_VERTEX: usize = 0;
const TYPE_TEXCOORD0: usize = 2;
const TYPE_COLOR: usize = 6;

/// The per-draw descriptor the viewer fills (POD, `#[repr(C)]`, mirrored in C++).
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct DrawDesc {
    pub mode: u32,       // LLRender: TRIANGLES=0, TRIANGLE_STRIP=1, ...
    pub count: u32,      // indices (indexed) or vertices (arrays)
    pub offset: u32,     // index offset / first vertex
    pub indexed: u32,    // bool
    pub typemask: u32,   // LLVertexBuffer type mask
    pub num_verts: u32,  // vertices in the buffer (drives calcOffsets)
    pub vtx_bytes: u32,  // size of the vertex blob
    pub idx_bytes: u32,  // size of the index blob
    pub tex0: u32,       // bound diffuse texture id (unit 0)
    pub depth_test: u32,
    pub depth_write: u32,
    pub blend: u32,
    pub mvp: [f32; 16],  // projection * modelview at draw time
    pub offsets: [u32; 16], // the viewer's own mOffsets[] (authoritative layout)
    pub color: [f32; 4],    // the bound shader's diffuse colour for THIS draw
    pub tex: [u32; 4],      // texture units 0..3 (indexed textures, chosen by position.w)
}

pub fn calc_offsets(typemask: u32, num_verts: u32) -> [u32; 14] {
    let mut offsets = [u32::MAX; 14];
    let mut offset = 0u32;
    for i in 0..13 {
        if typemask & (1 << i) != 0 && TYPE_SIZES[i] > 0 {
            offsets[i] = offset;
            offset += TYPE_SIZES[i] * num_verts;
            offset = (offset + 0xF) & !0xF;
        }
    }
    offsets
}

/// GL clip [-1,1] -> wgpu [0,1] depth remap (proven in the replay).
pub fn depth_fix() -> Mat4 {
    Mat4::from_cols_array(&[
        1.0, 0.0, 0.0, 0.0, //
        0.0, 1.0, 0.0, 0.0, //
        0.0, 0.0, 0.5, 0.0, //
        0.0, 0.0, 0.5, 1.0,
    ])
}

/// One submitted draw, resources already uploaded.
struct Queued {
    bind_key: (usize, u32),
    vptr: usize,
    iptr: Option<usize>,
    icount: u32,
    voffsets: [u32; 14],
    pipe_key: (bool, bool, bool),
    first: u32,
    vcount: u32,
}

pub struct LiveRenderer {
    bgl: wgpu::BindGroupLayout,
    layout: wgpu::PipelineLayout,
    vs: wgpu::ShaderModule,
    fs: wgpu::ShaderModule,
    pipelines: HashMap<(bool, bool, bool), wgpu::RenderPipeline>,
    sampler: wgpu::Sampler,
    white: wgpu::TextureView,
    textures: HashMap<u32, (wgpu::Texture, wgpu::TextureView)>,
    queued: Vec<Queued>,
    /// GPU copies of the viewer's vertex/index buffers, keyed by its CPU-shadow pointer.
    /// The viewer tells us when one changes (flush_vbo -> fsr_buffer_dirty), so we upload
    /// each buffer ONCE instead of re-uploading megabytes every frame (the CPU hog).
    geo: HashMap<usize, (wgpu::Buffer, u64)>,
    /// Reusable per-slot uniform buffers + bind groups (80 bytes each, thousands/frame).
    ubos: Vec<wgpu::Buffer>,
    binds: HashMap<(usize, u32), wgpu::BindGroup>,
    slot: usize,
    // Neutral attribute fallbacks: when a draw's typemask lacks TEXCOORD0/COLOR, binding
    // the POSITION block in their place reinterprets float positions as uv/RGBA8 -- which
    // painted the login text red/orange. Bind these instead: uv (0,0), colour opaque white.
    uv_zero: wgpu::Buffer,
    color_white: wgpu::Buffer,
    depth: Option<(wgpu::TextureView, u32, u32)>,
    format: wgpu::TextureFormat,
    pub submitted: u64,
    pub drawn: u64,
    /// Current diffuse colour (viewer's "color" uniform, captured by the null-GL stub).
    /// UI text/background colour lives here, not in vertex colours.
    pub cur_color: [f32; 4],
}

impl LiveRenderer {
    pub fn new(device: &wgpu::Device, queue: &wgpu::Queue, format: wgpu::TextureFormat) -> LiveRenderer {
        let bgl = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("live-bgl"),
            entries: &[
                wgpu::BindGroupLayoutEntry {
                    binding: 0,
                    // BOTH stages: the vertex shader reads mvp, the fragment shader reads
                    // the diffuse colour from the same block (vertex-only failed validation
                    // -> every pipeline dropped -> 0 draws rendered).
                    visibility: wgpu::ShaderStages::VERTEX_FRAGMENT,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Uniform,
                        has_dynamic_offset: false,
                        min_binding_size: None,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 1,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture { sample_type: wgpu::TextureSampleType::Float { filterable: true }, view_dimension: wgpu::TextureViewDimension::D2, multisampled: false },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 2,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
                    count: None,
                },
            ],
        });
        let layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("live-pl"),
            bind_group_layouts: &[&bgl],
            push_constant_ranges: &[],
        });
        // SPIR-V precompiled by glslc (no runtime compiler in the DLL); regenerate from
        // shaders/live.{vert,frag} via: glslc --target-env=vulkan1.2 live.vert -o live.vert.spv
        let vs = device.create_shader_module(wgpu::include_spirv!("../shaders/live.vert.spv"));
        let fs = device.create_shader_module(wgpu::include_spirv!("../shaders/live.frag.spv"));
        let sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("live-samp"),
            address_mode_u: wgpu::AddressMode::Repeat,
            address_mode_v: wgpu::AddressMode::Repeat,
            mag_filter: wgpu::FilterMode::Linear,
            min_filter: wgpu::FilterMode::Linear,
            mipmap_filter: wgpu::FilterMode::Nearest,
            ..Default::default()
        });
        let white_tex = device.create_texture_with_data(
            queue,
            &wgpu::TextureDescriptor {
                label: Some("white"),
                size: wgpu::Extent3d { width: 1, height: 1, depth_or_array_layers: 1 },
                mip_level_count: 1,
                sample_count: 1,
                dimension: wgpu::TextureDimension::D2,
                format: wgpu::TextureFormat::Rgba8UnormSrgb,
                usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
                view_formats: &[],
            },
            wgpu::util::TextureDataOrder::LayerMajor,
            &[255u8, 255, 255, 255],
        );
        const FALLBACK_VERTS: usize = 1 << 16;
        let uv_zero = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label: Some("uv-zero"),
            contents: &vec![0u8; FALLBACK_VERTS * 8],
            usage: wgpu::BufferUsages::VERTEX,
        });
        let color_white = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label: Some("color-white"),
            contents: &vec![0xFFu8; FALLBACK_VERTS * 4],
            usage: wgpu::BufferUsages::VERTEX,
        });
        LiveRenderer {
            bgl,
            layout,
            vs,
            fs,
            pipelines: HashMap::new(),
            sampler,
            white: white_tex.create_view(&wgpu::TextureViewDescriptor::default()),
            textures: HashMap::new(),
            queued: Vec::new(),
            geo: HashMap::new(),
            ubos: Vec::new(),
            binds: HashMap::new(),
            slot: 0,
            uv_zero,
            color_white,
            depth: None,
            format,
            submitted: 0,
            drawn: 0,
            cur_color: [1.0, 1.0, 1.0, 1.0],
        }
    }

    pub fn upload_texture(&mut self, device: &wgpu::Device, queue: &wgpu::Queue, id: u32, w: u32, h: u32, rgba: &[u8]) {
        if id == 0 || w == 0 || h == 0 || rgba.len() < (w * h * 4) as usize {
            return;
        }
        let t = device.create_texture_with_data(
            queue,
            &wgpu::TextureDescriptor {
                label: Some("live-tex"),
                size: wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 },
                mip_level_count: 1,
                sample_count: 1,
                dimension: wgpu::TextureDimension::D2,
                format: wgpu::TextureFormat::Rgba8UnormSrgb,
                usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
                view_formats: &[],
            },
            wgpu::util::TextureDataOrder::LayerMajor,
            &rgba[..(w * h * 4) as usize],
        );
        let v = t.create_view(&wgpu::TextureViewDescriptor::default());
        self.textures.insert(id, (t, v));
    }

    /// Patch a sub-rect of an existing texture IN PLACE (glTexSubImage2D). Font atlases
    /// are built glyph-by-glyph this way; re-uploading a whole (re-zeroed) atlas per glyph
    /// erased every glyph already written -- measured as blank login fields.
    pub fn upload_subtexture(&mut self, queue: &wgpu::Queue, id: u32, x: u32, y: u32, w: u32, h: u32, rgba: &[u8]) -> bool {
        let Some((tex, _)) = self.textures.get(&id) else { return false };
        if w == 0 || h == 0 || rgba.len() < (w * h * 4) as usize {
            return false;
        }
        let size = tex.size();
        if x + w > size.width || y + h > size.height {
            return false;
        }
        queue.write_texture(
            wgpu::ImageCopyTexture {
                texture: tex,
                mip_level: 0,
                origin: wgpu::Origin3d { x, y, z: 0 },
                aspect: wgpu::TextureAspect::All,
            },
            &rgba[..(w * h * 4) as usize],
            wgpu::ImageDataLayout { offset: 0, bytes_per_row: Some(w * 4), rows_per_image: Some(h) },
            wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 },
        );
        true
    }

    pub fn begin(&mut self) {
        self.queued.clear();
        self.slot = 0;
    }

    /// The viewer wrote to this CPU-shadow buffer: drop our GPU copy so it re-uploads once.
    pub fn invalidate(&mut self, ptr: usize) {
        self.geo.remove(&ptr);
    }

    /// Upload-once helper: returns a cached GPU buffer for `ptr`, creating it if needed.
    fn geo_buffer(&mut self, device: &wgpu::Device, ptr: usize, bytes: &[u8], usage: wgpu::BufferUsages) -> bool {
        if let Some((_, len)) = self.geo.get(&ptr) {
            if *len == bytes.len() as u64 {
                return true;
            }
        }
        let mut padded;
        let data = if bytes.len() % 4 == 0 {
            bytes
        } else {
            padded = bytes.to_vec();
            while padded.len() % 4 != 0 { padded.push(0); }
            &padded[..]
        };
        let b = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label: Some("geo"),
            contents: data,
            usage,
        });
        self.geo.insert(ptr, (b, bytes.len() as u64));
        true
    }

    /// Queue one draw. Vertex/index bytes are copied into GPU buffers (the viewer's CPU
    /// shadow is reused next frame, so we cannot hold the pointers).
    pub fn submit(&mut self, device: &wgpu::Device, queue: &wgpu::Queue, d: &DrawDesc, vtx: &[u8], idx: &[u8]) {
        self.submitted += 1;
        if !(d.mode == 0 || d.mode == 1) || d.num_verts == 0 || d.typemask & 1 == 0 || vtx.is_empty() {
            return;
        }
        // Geometry uploaded ONCE per buffer and reused until the viewer invalidates it
        // (flush_vbo -> fsr_buffer_dirty). Re-uploading every frame was the CPU hog.
        let vptr = vtx.as_ptr() as usize;
        if !self.geo_buffer(device, vptr, vtx, wgpu::BufferUsages::VERTEX) {
            return;
        }
        let (iptr, icount, first, vcount) = if d.indexed != 0 && !idx.is_empty() {
            let base_ptr = idx.as_ptr() as usize;
            if d.mode == 1 {
                let key = base_ptr ^ 0x5311_D000 ^ (d.offset as usize) ^ ((d.count as usize) << 20);
                let src: Vec<u16> = idx.chunks_exact(2).map(|b| u16::from_le_bytes([b[0], b[1]])).collect();
                let take: Vec<u16> = src.iter().skip(d.offset as usize).take(d.count as usize).copied().collect();
                let list = strip_to_list(&take);
                if list.is_empty() {
                    return;
                }
                let n = list.len() as u32;
                if !self.geo_buffer(device, key, bytemuck::cast_slice(&list), wgpu::BufferUsages::INDEX) {
                    return;
                }
                (Some(key), n, 0, 0)
            } else {
                if !self.geo_buffer(device, base_ptr, idx, wgpu::BufferUsages::INDEX) {
                    return;
                }
                (Some(base_ptr), d.count, d.offset, 0)
            }
        } else if d.mode == 1 {
            let seq: Vec<u16> = (d.offset as u16..(d.offset + d.count) as u16).collect();
            let list = strip_to_list(&seq);
            if list.is_empty() {
                return;
            }
            let key = 0xA51D_0000 ^ (d.offset as usize) ^ ((d.count as usize) << 20);
            let n = list.len() as u32;
            if !self.geo_buffer(device, key, bytemuck::cast_slice(&list), wgpu::BufferUsages::INDEX) {
                return;
            }
            (Some(key), n, 0, 0)
        } else {
            (None, 0, d.offset, d.count)
        };

        // Per-slot UBO + bind group, written (not allocated) each frame.
        let slot = self.slot;
        self.slot += 1;
        if slot >= self.ubos.len() {
            let b = device.create_buffer(&wgpu::BufferDescriptor {
                label: Some("live-ubo"),
                size: 80,
                usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
                mapped_at_creation: false,
            });
            self.ubos.push(b);
        }
        let mvp = depth_fix() * Mat4::from_cols_array(&d.mvp);
        let mut ubo_data = [0f32; 20];
        ubo_data[..16].copy_from_slice(&mvp.to_cols_array());
        ubo_data[16..].copy_from_slice(&d.color);
        queue.write_buffer(&self.ubos[slot], 0, bytemuck::cast_slice(&ubo_data));

        let bind_key = (slot, d.tex0);
        if !self.binds.contains_key(&bind_key) {
            let tv = self.textures.get(&d.tex0).map(|(_, v)| v).unwrap_or(&self.white);
            let bg = device.create_bind_group(&wgpu::BindGroupDescriptor {
                label: Some("live-bind"),
                layout: &self.bgl,
                entries: &[
                    wgpu::BindGroupEntry { binding: 0, resource: self.ubos[slot].as_entire_binding() },
                    wgpu::BindGroupEntry { binding: 1, resource: wgpu::BindingResource::TextureView(tv) },
                    wgpu::BindGroupEntry { binding: 2, resource: wgpu::BindingResource::Sampler(&self.sampler) },
                ],
            });
            self.binds.insert(bind_key, bg);
        }

        let pipe_key = (d.depth_test != 0, d.depth_write != 0, d.blend != 0);
        self.ensure_pipeline(device, pipe_key);
        let voffsets = {
            let mut o = [u32::MAX; 14];
            let has = d.offsets.iter().take(14).any(|&x| x != 0);
            for i in 0..14 {
                if d.typemask & (1 << i) != 0 {
                    o[i] = if has { d.offsets[i] } else { calc_offsets(d.typemask, d.num_verts)[i] };
                }
            }
            o
        };
        self.queued.push(Queued { bind_key, vptr, iptr, icount, voffsets, pipe_key, first, vcount });
    }

    fn ensure_pipeline(&mut self, device: &wgpu::Device, key: (bool, bool, bool)) {
        if self.pipelines.contains_key(&key) {
            return;
        }
        let vlayouts = [
            wgpu::VertexBufferLayout {
                array_stride: 16,
                step_mode: wgpu::VertexStepMode::Vertex,
                attributes: &[wgpu::VertexAttribute { offset: 0, shader_location: 0, format: wgpu::VertexFormat::Float32x4 }],
            },
            wgpu::VertexBufferLayout {
                array_stride: 8,
                step_mode: wgpu::VertexStepMode::Vertex,
                attributes: &[wgpu::VertexAttribute { offset: 0, shader_location: 1, format: wgpu::VertexFormat::Float32x2 }],
            },
            wgpu::VertexBufferLayout {
                array_stride: 4,
                step_mode: wgpu::VertexStepMode::Vertex,
                attributes: &[wgpu::VertexAttribute { offset: 0, shader_location: 2, format: wgpu::VertexFormat::Unorm8x4 }],
            },
        ];
        let p = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("live"),
            layout: Some(&self.layout),
            vertex: wgpu::VertexState { module: &self.vs, entry_point: "main", buffers: &vlayouts },
            fragment: Some(wgpu::FragmentState {
                module: &self.fs,
                entry_point: "main",
                targets: &[Some(wgpu::ColorTargetState {
                    format: self.format,
                    blend: if key.2 { Some(wgpu::BlendState::ALPHA_BLENDING) } else { Some(wgpu::BlendState::REPLACE) },
                    write_mask: wgpu::ColorWrites::ALL,
                })],
            }),
            primitive: wgpu::PrimitiveState {
                topology: wgpu::PrimitiveTopology::TriangleList,
                cull_mode: None,
                ..Default::default()
            },
            depth_stencil: Some(wgpu::DepthStencilState {
                format: wgpu::TextureFormat::Depth32Float,
                depth_write_enabled: key.1,
                depth_compare: if key.0 { wgpu::CompareFunction::LessEqual } else { wgpu::CompareFunction::Always },
                stencil: wgpu::StencilState::default(),
                bias: wgpu::DepthBiasState::default(),
            }),
            multisample: wgpu::MultisampleState::default(),
            multiview: None,
        });
        self.pipelines.insert(key, p);
    }

    fn ensure_depth(&mut self, device: &wgpu::Device, w: u32, h: u32) {
        let need = match &self.depth {
            Some((_, dw, dh)) => *dw != w || *dh != h,
            None => true,
        };
        if need {
            let t = device.create_texture(&wgpu::TextureDescriptor {
                label: Some("live-depth"),
                size: wgpu::Extent3d { width: w.max(1), height: h.max(1), depth_or_array_layers: 1 },
                mip_level_count: 1,
                sample_count: 1,
                dimension: wgpu::TextureDimension::D2,
                format: wgpu::TextureFormat::Depth32Float,
                usage: wgpu::TextureUsages::RENDER_ATTACHMENT,
                view_formats: &[],
            });
            self.depth = Some((t.create_view(&wgpu::TextureViewDescriptor::default()), w, h));
        }
    }

    /// Render every queued draw into `target` and return how many were drawn.
    pub fn flush(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        target: &wgpu::TextureView,
        w: u32,
        h: u32,
    ) -> u64 {
        self.ensure_depth(device, w, h);
        let depth_view = match &self.depth {
            Some((v, _, _)) => v,
            None => return 0,
        };
        let mut enc = device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("live") });
        {
            let mut rp = enc.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("live-pass"),
                color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                    view: target,
                    resolve_target: None,
                    ops: wgpu::Operations {
                        load: wgpu::LoadOp::Clear(wgpu::Color { r: 0.09, g: 0.35, b: 0.4, a: 1.0 }),
                        store: wgpu::StoreOp::Store,
                    },
                })],
                depth_stencil_attachment: Some(wgpu::RenderPassDepthStencilAttachment {
                    view: depth_view,
                    depth_ops: Some(wgpu::Operations { load: wgpu::LoadOp::Clear(1.0), store: wgpu::StoreOp::Store }),
                    stencil_ops: None,
                }),
                timestamp_writes: None,
                occlusion_query_set: None,
            });
            for q in &self.queued {
                let Some(p) = self.pipelines.get(&q.pipe_key) else { continue };
                let Some(bind) = self.binds.get(&q.bind_key) else { continue };
                let Some((vbuf, _)) = self.geo.get(&q.vptr) else { continue };
                rp.set_pipeline(p);
                rp.set_bind_group(0, bind, &[]);
                let vo = q.voffsets;
                rp.set_vertex_buffer(0, vbuf.slice(vo[TYPE_VERTEX] as u64..));
                if vo[TYPE_TEXCOORD0] != u32::MAX {
                    rp.set_vertex_buffer(1, vbuf.slice(vo[TYPE_TEXCOORD0] as u64..));
                } else {
                    rp.set_vertex_buffer(1, self.uv_zero.slice(..));
                }
                if vo[TYPE_COLOR] != u32::MAX {
                    rp.set_vertex_buffer(2, vbuf.slice(vo[TYPE_COLOR] as u64..));
                } else {
                    rp.set_vertex_buffer(2, self.color_white.slice(..));
                }
                if let Some(ip) = q.iptr {
                    let Some((ib, _)) = self.geo.get(&ip) else { continue };
                    rp.set_index_buffer(ib.slice(..), wgpu::IndexFormat::Uint16);
                    rp.draw_indexed(q.first..q.first + q.icount, 0, 0..1);
                } else {
                    rp.draw(q.first..q.first + q.vcount, 0..1);
                }
            }
        }
        queue.submit([enc.finish()]);
        let n = self.queued.len() as u64;
        self.drawn += n;
        self.queued.clear();
        n
    }
}

fn strip_to_list(src: &[u16]) -> Vec<u16> {
    let mut list = Vec::new();
    for k in 2..src.len() {
        if k % 2 == 0 {
            list.extend_from_slice(&[src[k - 2], src[k - 1], src[k]]);
        } else {
            list.extend_from_slice(&[src[k - 1], src[k - 2], src[k]]);
        }
    }
    list
}

