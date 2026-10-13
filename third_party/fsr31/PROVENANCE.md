# Local DX11 x86 SDK package

Imported from `C:/Users/Newbie/Documents/GitHub/FSR3.1 x86` on 2026-10-13.
Origin: OptiScaler/FidelityFX-SDK-DX11, optiscaler-build,
commit 9b04fa49abae5aa341c0007af13c8ba1b4ad9913 plus included x86.patch.
Release backend, FSR3 upscaler, frame interpolation and optical flow are linked.
/MD, MSVC 19.51. Original backend implementation is overridden by the copied
`backend/ffx_dx11.cpp`; the library supplies shader blobs/checksum code.
Local fixes add structured-buffer stride/misc flags and SRV alongside UAV for
FI_Counters, and safe truncation in the HRESULT error formatter. The original
package crashes at FI_Counters creation; the local probe confirms these fixes
allow all 7 OF and 11 FI compute pipelines to initialize.
Headers/licenses/patch accompany the binaries. No DLL injection from OptiScaler,
No SDK-provided swapchain scheduler is used (DX11 callback is a success-only stub).

Release SHA256:

- ffx_backend_dx11_x86.lib: 651E866EE7412C0397C9748B01C04D0A31EFCBABEB25A5977329DE7FAC78765B
- ffx_fsr3upscaler_x86.lib: 35955C68AE92B986A4042F70138EEB0EA7237886A403FC756B3C6B48EE184A3A
- ffx_frameinterpolation_x86.lib: 1ABCA8889053CB1A0E5308D67548092276E94F6B89920CD0B4C5F8BB0D98C982
- ffx_opticalflow_x86.lib: 5AAC20CB73537A7BCDDDFC830C341FA8240D8FAC77FDDEE0DAA36DB22809145B
