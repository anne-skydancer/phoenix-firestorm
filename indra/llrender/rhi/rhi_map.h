/**
 * @file rhi_map.h
 * @brief GLenum -> Rhi* reverse maps for routing raw-GL state calls through gRHI.
 *
 * <FSVulkan P2 J2 sweep> Single source of truth for the caller-side (newview) reverse
 * mapping. Each helper is the exact inverse of the GL backend's forward map in
 * rhi_gl.cpp (gl_compare / gl_cull_face / gl_polygon_mode / gl_stencil_op), so
 *   gl_x(rhi_x_from_gl(v)) == v
 * holds by construction -- routing a leak through gRHI emits the identical GL call.
 *
 * C++ header (inline helpers), unlike the C-ABI rhi.h. Include where a newview TU
 * routes fixed-function state; pointsize/polygonoffset take floats and need no map.
 */
#ifndef LL_RHI_MAP_H
#define LL_RHI_MAP_H

#include "llglheaders.h"   // GLenum + GL_* constants
#include "rhi/rhi.h"       // Rhi* enums + gRHI

// depth/stencil compare func
inline RhiCompare rhi_cmp_from_gl(GLenum f)
{
    switch (f)
    {
        case GL_NEVER:    return RHI_CMP_NEVER;
        case GL_LESS:     return RHI_CMP_LESS;
        case GL_EQUAL:    return RHI_CMP_EQUAL;
        case GL_LEQUAL:   return RHI_CMP_LEQUAL;
        case GL_GREATER:  return RHI_CMP_GREATER;
        case GL_NOTEQUAL: return RHI_CMP_NOTEQUAL;
        case GL_GEQUAL:   return RHI_CMP_GEQUAL;
        case GL_ALWAYS:   return RHI_CMP_ALWAYS;
        default:          return RHI_CMP_LESS;   // GL default
    }
}

// glCullFace target
inline RhiCullFace rhi_cull_face_from_gl(GLenum face)
{
    switch (face)
    {
        case GL_BACK:           return RHI_CF_BACK;
        case GL_FRONT:          return RHI_CF_FRONT;
        case GL_FRONT_AND_BACK: return RHI_CF_FRONT_AND_BACK;
        default:                return RHI_CF_BACK;
    }
}

// glPolygonMode fill mode
inline RhiPolygonMode rhi_polygon_mode_from_gl(GLenum mode)
{
    switch (mode)
    {
        case GL_FILL:  return RHI_PM_FILL;
        case GL_LINE:  return RHI_PM_LINE;
        case GL_POINT: return RHI_PM_POINT;
        default:       return RHI_PM_FILL;
    }
}

// glStencilOp action
inline RhiStencilOp rhi_stencil_op_from_gl(GLenum op)
{
    switch (op)
    {
        case GL_KEEP:      return RHI_SO_KEEP;
        case GL_ZERO:      return RHI_SO_ZERO;
        case GL_REPLACE:   return RHI_SO_REPLACE;
        case GL_INCR:      return RHI_SO_INCR;
        case GL_DECR:      return RHI_SO_DECR;
        case GL_INVERT:    return RHI_SO_INVERT;
        case GL_INCR_WRAP: return RHI_SO_INCR_WRAP;
        case GL_DECR_WRAP: return RHI_SO_DECR_WRAP;
        default:           return RHI_SO_KEEP;
    }
}

// glBindBuffer / glBufferData / glBufferSubData target (R2 · J3)
inline RhiBufferTarget rhi_buftgt_from_gl(GLenum t)
{
    switch (t)
    {
        case GL_ARRAY_BUFFER:         return RHI_BUFTGT_VERTEX;
        case GL_ELEMENT_ARRAY_BUFFER: return RHI_BUFTGT_INDEX;
        case GL_UNIFORM_BUFFER:       return RHI_BUFTGT_UNIFORM;
        default:                      return RHI_BUFTGT_VERTEX;
    }
}

// glBufferData usage hint (R2 · J3)
inline RhiBufferHint rhi_bufhint_from_gl(GLenum usage)
{
    switch (usage)
    {
        case GL_STATIC_DRAW:  return RHI_BUFHINT_STATIC;
        case GL_DYNAMIC_DRAW: return RHI_BUFHINT_DYNAMIC;
        case GL_STREAM_DRAW:  return RHI_BUFHINT_STREAM;
        default:              return RHI_BUFHINT_STATIC;
    }
}

// index element type (R7 · J3) -- glDrawRangeElements type / mIndicesType
inline RhiIndexType rhi_idxtype_from_gl(GLenum t)
{
    switch (t)
    {
        case GL_UNSIGNED_SHORT: return RHI_IDX_U16;
        case GL_UNSIGNED_INT:   return RHI_IDX_U32;
        default:                return RHI_IDX_U16;
    }
}

#endif // LL_RHI_MAP_H
