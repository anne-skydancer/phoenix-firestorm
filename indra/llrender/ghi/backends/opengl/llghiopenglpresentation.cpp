/**
 * @file llghiopenglpresentation.cpp
 * @brief OpenGL implementation of the backend-neutral presentation lifecycle.
 */

#if !defined(_WIN32)
#error The R1 OpenGL presentation adapter currently supports Windows only
#endif

#include "llghiopenglpresentation.h"

#include "llghiopenglinfo.h"
#include "llgl.h"

#include <utility>

namespace LL::GHI
{
namespace
{

class OpenGLPresentationSurface final : public PresentationSurface
{
public:
    ~OpenGLPresentationSurface() override { shutdown(); }

    Status initialize(const PresentationCreateInfo& info)
    {
        if (!info.nativeWindow || info.width == 0 || info.height == 0)
        {
            return Status::failure(
                StatusCode::InvalidArgument,
                "OpenGL presentation requires a native window and non-zero extent");
        }
        if (!wglGetCurrentContext())
        {
            return Status::failure(
                StatusCode::InvalidState,
                "OpenGL presentation requires a current WGL context");
        }

        mWindow = static_cast<HWND>(info.nativeWindow);
        mDC = GetDC(mWindow);
        if (!mDC || mDC != wglGetCurrentDC())
        {
            if (mDC) ReleaseDC(mWindow, mDC);
            mDC = nullptr;
            mWindow = nullptr;
            return Status::failure(
                StatusCode::InvalidState,
                "The current WGL context does not belong to the supplied window");
        }
        mWidth = info.width;
        mHeight = info.height;
        mSnapshot = queryOpenGLRendererSnapshot();
        if (!mSnapshot.complete())
        {
            return Status::failure(
                StatusCode::BackendError,
                "OpenGL did not produce a complete renderer snapshot");
        }
        publishRendererSnapshot(mSnapshot);
        mSnapshotPublished = true;
        return Status::success();
    }

    Backend backend() const override { return Backend::OpenGL; }
    const RendererSnapshot& rendererSnapshot() const override { return mSnapshot; }

    Status presentClear(const ClearColor& color) override
    {
        if (!mDC)
        {
            return Status::failure(StatusCode::InvalidState, "OpenGL presentation is shut down");
        }
        if (mSuspended)
        {
            return Status::success();
        }
        if (wglGetCurrentDC() != mDC)
        {
            return Status::failure(
                StatusCode::InvalidState,
                "The OpenGL presentation context is not current on this thread");
        }
        glViewport(0, 0, static_cast<GLsizei>(mWidth), static_cast<GLsizei>(mHeight));
        glClearColor(color.red, color.green, color.blue, color.alpha);
        glClear(GL_COLOR_BUFFER_BIT);
        if (!SwapBuffers(mDC))
        {
            return Status::failure(StatusCode::BackendError, "SwapBuffers failed");
        }
        return Status::success();
    }

    Status resize(std::uint32_t width, std::uint32_t height) override
    {
        mWidth = width;
        mHeight = height;
        mSuspended = width == 0 || height == 0;
        return Status::success();
    }

    Status setSuspended(bool suspended) override
    {
        mSuspended = suspended;
        return Status::success();
    }

    Status shutdown() override
    {
        if (mDC)
        {
            ReleaseDC(mWindow, mDC);
            mDC = nullptr;
            mWindow = nullptr;
        }
        if (mSnapshotPublished)
        {
            const auto active = activeRendererSnapshot();
            if (active && *active == mSnapshot)
            {
                clearRendererSnapshot();
            }
            mSnapshotPublished = false;
        }
        return Status::success();
    }

private:
    HWND mWindow = nullptr;
    HDC mDC = nullptr;
    RendererSnapshot mSnapshot;
    std::uint32_t mWidth = 0;
    std::uint32_t mHeight = 0;
    bool mSuspended = false;
    bool mSnapshotPublished = false;
};

} // namespace

PresentationCreationResult createOpenGLPresentationSurface(
    const PresentationCreateInfo& info)
{
    auto surface = std::make_unique<OpenGLPresentationSurface>();
    Status status = surface->initialize(info);
    if (!status)
    {
        surface->shutdown();
        return { nullptr, std::move(status) };
    }
    return { std::move(surface), Status::success() };
}

} // namespace LL::GHI
