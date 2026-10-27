# Collects the licence texts of the third-party components that are built into the program (from the folders build-windows.ps1 downloads) into THIRD-PARTY-NOTICES.txt.
# Usage: tools\collect-notices.ps1 [-ThirdParty C:\caligo-chaser-build\third_party]
param([string]$ThirdParty = 'C:\caligo-chaser-build\third_party')
$T = $ThirdParty; $E = "$T\dynarmic\externals"; $out = Join-Path (Split-Path $PSScriptRoot) 'THIRD-PARTY-NOTICES.txt'
$bsd3 = "Copyright (c) 2007 MITSUNARI Shigeo and the Xbyak contributors`r`nAll rights reserved.`r`n`r`n" +
"Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:`r`n" +
" * Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.`r`n" +
" * Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.`r`n" +
" * Neither the name of the copyright owner nor the names of its contributors may be used to endorse or promote products derived from this software without specific prior written permission.`r`n" +
"THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS `"AS IS`" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE."
$parts = @(
    @('dynarmic 6.6.x - ARM JIT (0BSD)', "$T\dynarmic\LICENSE.txt"), @('mcl (MIT)', "$E\mcl\LICENSE"), @('fmt (MIT)', "$E\fmt\LICENSE.rst"), @('Zydis and Zycore (MIT)', "$E\zydis\LICENSE"),
    @('robin-map (MIT)', "$E\robin-map\LICENSE"), @('oaknut (MIT)', "$E\oaknut\LICENSE"), @('xbyak (BSD-3-Clause)', $null), @('Boost headers 1.84 (Boost Software License 1.0)', "$T\boost-1.84.0\LICENSE_1_0.txt"),
    @('SDL 2.30.9 (zlib)', "$T\SDL2-2.30.9\LICENSE.txt"), @('miniz 3.0.2 (MIT)', "$T\miniz\LICENSE"), @('stb_vorbis and stb_image (public domain / MIT)', "$T\stb\LICENSE") )
$sb = New-Object Text.StringBuilder; [void]$sb.AppendLine("THIRD-PARTY NOTICES - Caligo Chaser DX`r`nThe following open-source components are built into the program. Their licence texts follow.`r`n")
foreach ($p in $parts) {
    [void]$sb.AppendLine(('=' * 78)); [void]$sb.AppendLine($p[0]); [void]$sb.AppendLine(('=' * 78))
    if ($p[1]) { if (-not (Test-Path $p[1])) { throw "missing $($p[1])" }; [void]$sb.AppendLine(([IO.File]::ReadAllText($p[1]).TrimEnd())) } else { [void]$sb.AppendLine($bsd3) }
    [void]$sb.AppendLine()
}
[IO.File]::WriteAllText($out, $sb.ToString(), (New-Object Text.UTF8Encoding($false))); Write-Host "Wrote $out"
