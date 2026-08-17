/**
 * @file llghiopenglpresentation.h
 * @brief Private OpenGL presentation adapter factory.
 */

#ifndef LL_LLGHIOPENGLPRESENTATION_H
#define LL_LLGHIOPENGLPRESENTATION_H

#include "ghi/include/llghipresentation.h"

namespace LL::GHI
{

PresentationCreationResult createOpenGLPresentationSurface(
    const PresentationCreateInfo& info);

} // namespace LL::GHI

#endif // LL_LLGHIOPENGLPRESENTATION_H
