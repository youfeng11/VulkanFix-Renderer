"""Run with qrenderdoc --python, before its UI opens; report a real replay result."""
import json
import os
from pathlib import Path
import traceback
import renderdoc as rd


def check_capture(path, expect_clear=False):
    capture = rd.OpenCaptureFile()
    controller = None
    remote = None
    try:
        status = capture.OpenFile(str(path), '', None)
        if status != rd.ResultCode.Succeeded:
            raise RuntimeError(f'OpenFile: {status}')
        host = os.environ.get('VULKAN_FIX_REPLAY_HOST', '')
        if host:
            status, remote = rd.CreateRemoteServerConnection(host)
            if status != rd.ResultCode.Succeeded or remote is None:
                raise RuntimeError(f'Remote connection: {status}')
            remote_path = remote.CopyCaptureToRemote(str(path), None)
            if not remote_path:
                raise RuntimeError('Could not upload capture to the remote replay server')
            status, controller = remote.OpenCapture(0xffffffff, remote_path, rd.ReplayOptions(), None)
        else:
            status, controller = capture.OpenCapture(rd.ReplayOptions(), None)
        if status != rd.ResultCode.Succeeded or controller is None:
            raise RuntimeError(f'OpenCapture: {status}')
        actions = []

        def visit(nodes):
            for action in nodes:
                actions.append(action)
                visit(action.children)
        visit(controller.GetRootActions())
        if not actions:
            raise RuntimeError('Capture replay contains no actions')
        textures = controller.GetTextures()
        result = {'ok': True, 'capture': str(path), 'actions': len(actions),
                  'textures': len(textures), 'last_event': max(a.eventId for a in actions),
                  'replay_host': host or 'local'}
        controller.SetFrameEvent(result['last_event'], True)
        if expect_clear:
            candidates = [t for t in textures if t.width == 64 and t.height == 64
                          and t.format.Name() == 'R8G8B8A8_UNORM']
            if not candidates:
                raise RuntimeError('Expected 64x64 RGBA8 texture was not captured')
            expected = (51, 128, 204, 255)
            for texture in candidates:
                pixels = controller.GetTextureData(texture.resourceId, rd.Subresource())
                if len(pixels) == 64 * 64 * 4 and all(
                        abs(value - expected[i % 4]) <= 1 for i, value in enumerate(pixels)):
                    result['verified_pixels'] = 4096
                    result['first_pixel'] = list(pixels[:4])
                    break
            else:
                raise RuntimeError('Replayed texture does not match the GPU clear result')
        return result
    finally:
        if controller is not None:
            if remote is not None:
                remote.CloseCapture(controller)
            else:
                controller.Shutdown()
        if remote is not None:
            remote.ShutdownConnection()
        capture.Shutdown()


def main():
    report = Path(os.environ['VULKAN_FIX_REPLAY_REPORT'])
    try:
        capture = Path(os.environ['VULKAN_FIX_REPLAY_CAPTURE']).resolve()
        result = check_capture(capture, os.environ.get('VULKAN_FIX_REPLAY_EXPECT_CLEAR') == '1')
    except Exception as error:
        result = {'ok': False, 'error': str(error), 'traceback': traceback.format_exc()}
    try:
        report.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    except Exception:
        os._exit(1)
    # This command runs before qrenderdoc creates its main window.
    os._exit(0 if result['ok'] else 1)


main()
