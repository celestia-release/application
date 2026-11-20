# Generate LVGL fmt_txt bitmap fonts for Nova Home (Scheme B).
# Requires Windows + System.Drawing + a CJK font (Microsoft YaHei / SimSun).
# Usage: powershell -File gen_home_fonts.ps1
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$FontsDir = [System.IO.Path]::GetFullPath((Join-Path $ScriptDir "..\..\fonts"))
$Sizes = @(20, 28)
$SymFile = Join-Path $ScriptDir "home_symbols.txt"
# UTF-8 symbol list (avoid PowerShell source encoding issues)
$Symbols = [System.IO.File]::ReadAllText($SymFile, [System.Text.UTF8Encoding]::new($false))
$cpList = New-Object System.Collections.Generic.List[int]
foreach ($ch in $Symbols.ToCharArray()) {
    $cp = [int][char]$ch
    if ($cp -ge 0x20 -and -not $cpList.Contains($cp)) { [void]$cpList.Add($cp) }
}
# Ensure basic ASCII printable (FORMAT0_TINY 0x20-0x7E)
for ($c = 0x20; $c -le 0x7E; $c++) {
    if (-not $cpList.Contains($c)) { [void]$cpList.Add($c) }
}
$cpList.Sort()

$ff = $null
foreach ($n in @("Microsoft YaHei", "SimSun", "SimHei", "Noto Sans SC", "Source Han Sans SC")) {
    try { $ff = New-Object Drawing.FontFamily($n); Write-Host "font family: $n"; break } catch {}
}
if (-not $ff) { throw "No CJK font family found" }

function Pack-Bpp4([byte[,]]$gray, [int]$w, [int]$h) {
    # LVGL stride=0: continuous nibble stream across the whole glyph (NOT per-line padded).
    # Odd box_w must carry the leftover nibble into the next row — matching lv_font_conv.
    $bytes = New-Object System.Collections.Generic.List[byte]
    $haveHi = $false
    $cur = [byte]0
    for ($y = 0; $y -lt $h; $y++) {
        for ($x = 0; $x -lt $w; $x++) {
            $n = [byte](($gray[$x, $y] -shr 4) -band 0xF)
            if (-not $haveHi) {
                $cur = [byte](($n -shl 4) -band 0xF0)
                $haveHi = $true
            } else {
                [void]$bytes.Add([byte]($cur -bor $n))
                $haveHi = $false
            }
        }
    }
    if ($haveHi) { [void]$bytes.Add($cur) }
    return $bytes.ToArray()
}

function Emit-Font([int]$size) {
    $style = [Drawing.FontStyle]::Regular
    $font = New-Object Drawing.Font($ff, $size, $style, [Drawing.GraphicsUnit]::Pixel)
    $em = [double]$ff.GetEmHeight($style)
    $ascentPx = [int][Math]::Round($ff.GetCellAscent($style) * $size / $em)
    $descentPx = [int][Math]::Round($ff.GetCellDescent($style) * $size / $em)
    if ($ascentPx -lt 1) { $ascentPx = $size }
    if ($descentPx -lt 0) { $descentPx = 0 }

    $sf = [Drawing.StringFormat]::GenericTypographic
    $sf.FormatFlags = $sf.FormatFlags -bor [Drawing.StringFormatFlags]::MeasureTrailingSpaces
    $sf.Alignment = [Drawing.StringAlignment]::Near
    $sf.LineAlignment = [Drawing.StringAlignment]::Near

    $glyphs = @()
    $bitmapAll = New-Object System.Collections.Generic.List[byte]
    $bitmapIndex = 0
    $maxBoxH = 0
    $minOfsY = 0
    $maxOfsY = 0

    # Canvas: baseline at y = ascentPx (LVGL-compatible)
    $canvasW = [Math]::Max(8, $size * 3)
    $canvasH = [Math]::Max(8, $ascentPx + $descentPx + 4)

    foreach ($cp in $cpList) {
        $ch = [char]$cp
        $str = [string]$ch

        # Space / empty: match official Montserrat (box 0x0, advance only)
        if ($cp -eq 0x20) {
            $adv = [int]([Math]::Round($size * 0.35) * 16)
            $glyphs += [pscustomobject]@{
                Cp = $cp; BitmapIndex = 0; Adv = $adv
                BoxW = 0; BoxH = 0; OfsX = 0; OfsY = 0
            }
            continue
        }

        $bmp = New-Object Drawing.Bitmap $canvasW, $canvasH
        $g = [Drawing.Graphics]::FromImage($bmp)
        $g.Clear([Drawing.Color]::Black)
        $g.TextRenderingHint = [Drawing.Text.TextRenderingHint]::AntiAliasGridFit
        $g.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::AntiAlias
        # Draw so typographic baseline sits at ascentPx
        $g.DrawString($str, $font, [Drawing.Brushes]::White, 0.0, 0.0, $sf)
        $msz = $g.MeasureString($str, $font, [Drawing.PointF]::new(0, 0), $sf)
        $g.Dispose()

        $minX = $canvasW; $minY = $canvasH; $maxX = -1; $maxY = -1
        for ($y = 0; $y -lt $canvasH; $y++) {
            for ($x = 0; $x -lt $canvasW; $x++) {
                $p = $bmp.GetPixel($x, $y)
                $v = [int](0.299 * $p.R + 0.587 * $p.G + 0.114 * $p.B)
                if ($v -gt 12) {
                    if ($x -lt $minX) { $minX = $x }
                    if ($y -lt $minY) { $minY = $y }
                    if ($x -gt $maxX) { $maxX = $x }
                    if ($y -gt $maxY) { $maxY = $y }
                }
            }
        }

        if ($maxX -lt 0) {
            $adv = [int]([Math]::Max(1, [Math]::Ceiling($msz.Width)) * 16)
            $glyphs += [pscustomobject]@{
                Cp = $cp; BitmapIndex = 0; Adv = $adv
                BoxW = 0; BoxH = 0; OfsX = 0; OfsY = 0
            }
            $bmp.Dispose()
            continue
        }

        $boxW = $maxX - $minX + 1
        $boxH = $maxY - $minY + 1
        $gray = New-Object 'byte[,]' $boxW, $boxH
        for ($y = 0; $y -lt $boxH; $y++) {
            for ($x = 0; $x -lt $boxW; $x++) {
                $p = $bmp.GetPixel($minX + $x, $minY + $y)
                $gray[$x, $y] = [byte](0.299 * $p.R + 0.587 * $p.G + 0.114 * $p.B)
            }
        }
        $packed = Pack-Bpp4 $gray $boxW $boxH
        $bmp.Dispose()

        # LVGL: letter_y1 = baseline_y - box_h - ofs_y  =>  ofs_y = baseline - box_bottom
        # With DrawString at (0,0), GDI baseline ≈ ascentPx for most fonts.
        $baselineY = $ascentPx
        $boxBottom = $minY + $boxH
        $ofsY = $baselineY - $boxBottom
        $ofsX = $minX
        $advW = [int]([Math]::Max($boxW + 1, [Math]::Ceiling($msz.Width)) * 16)
        # CJK: prefer square advance ≈ size
        if ($cp -ge 0x4E00) {
            $advW = [int]($size * 16)
            # Center horizontally a bit if crop is narrow
            if ($boxW -lt $size) {
                $ofsX = [int](($size - $boxW) / 2)
            } else {
                $ofsX = 0
            }
        }

        if ($boxH -gt $maxBoxH) { $maxBoxH = $boxH }
        if ($ofsY -lt $minOfsY) { $minOfsY = $ofsY }
        if ($ofsY -gt $maxOfsY) { $maxOfsY = $ofsY }

        $glyphs += [pscustomobject]@{
            Cp = $cp
            BitmapIndex = $bitmapIndex
            Adv = $advW
            BoxW = $boxW
            BoxH = $boxH
            OfsX = $ofsX
            OfsY = $ofsY
        }
        foreach ($b in $packed) { [void]$bitmapAll.Add([byte]$b) }
        $bitmapIndex = $bitmapAll.Count
    }
    $font.Dispose()

    # Match Montserrat-style metrics: compact line box
    $lineHeight = [Math]::Max($size + 2, $ascentPx + $descentPx + 1)
    if ($maxBoxH + 2 -gt $lineHeight) { $lineHeight = $maxBoxH + 2 }
    $baseLine = [Math]::Max(2, $descentPx + 1)
    # If glyphs hang below baseline (negative ofs_y), enlarge descent
    if (-$minOfsY + 1 -gt $baseLine) { $baseLine = -$minOfsY + 1 }
    if ($ascentPx + $baseLine -gt $lineHeight) { $lineHeight = $ascentPx + $baseLine }

    $name = "lv_font_home_$size"
    $guard = "LV_FONT_HOME_$size"
    $sb = New-Object System.Text.StringBuilder
    [void]$sb.AppendLine("/* Auto-generated by gen_home_fonts.ps1 — Nova Home Scheme B. Do not edit. */")
    [void]$sb.AppendLine("#ifdef LV_LVGL_H_INCLUDE_SIMPLE")
    [void]$sb.AppendLine("#include `"lvgl.h`"")
    [void]$sb.AppendLine("#else")
    [void]$sb.AppendLine("#include `"lvgl.h`"")
    [void]$sb.AppendLine("#endif")
    [void]$sb.AppendLine("")
    [void]$sb.AppendLine("#ifndef $guard")
    [void]$sb.AppendLine("#define $guard 1")
    [void]$sb.AppendLine("#endif")
    [void]$sb.AppendLine("")
    [void]$sb.AppendLine("#if $guard")
    [void]$sb.AppendLine("static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {")
    if ($bitmapAll.Count -eq 0) {
        [void]$sb.AppendLine("    0x00,")
    } else {
        $line = "    "
        $col = 0
        foreach ($b in $bitmapAll) {
            $line += ("0x{0:x2}, " -f $b)
            $col++
            if ($col -ge 12) {
                [void]$sb.AppendLine($line.TrimEnd())
                $line = "    "
                $col = 0
            }
        }
        if ($col -gt 0) { [void]$sb.AppendLine($line.TrimEnd()) }
    }
    [void]$sb.AppendLine("};")
    [void]$sb.AppendLine("")
    [void]$sb.AppendLine("static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {")
    [void]$sb.AppendLine("    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id=0 reserved */,")
    foreach ($g in $glyphs) {
        [void]$sb.AppendLine(("    {{.bitmap_index = {0}, .adv_w = {1}, .box_w = {2}, .box_h = {3}, .ofs_x = {4}, .ofs_y = {5}}} /* U+{6:X4} */," -f `
            $g.BitmapIndex, $g.Adv, $g.BoxW, $g.BoxH, $g.OfsX, $g.OfsY, $g.Cp))
    }
    [void]$sb.AppendLine("};")
    [void]$sb.AppendLine("")

    $asciiCount = 0
    foreach ($g in $glyphs) {
        if ($g.Cp -ge 0x20 -and $g.Cp -le 0x7E) { $asciiCount++ } else { break }
    }
    if ($asciiCount -ne 95) {
        throw "ASCII cmap requires exactly 95 glyphs (0x20-0x7E), got $asciiCount"
    }
    $cjk = @($glyphs | Where-Object { $_.Cp -gt 0x7E })
    if ($cjk.Count -gt 0) {
        $cjkStart = $cjk[0].Cp
        [void]$sb.AppendLine("static const uint16_t unicode_list_1[] = {")
        $line = "    "
        $col = 0
        foreach ($g in $cjk) {
            $line += ("0x{0:x}, " -f ($g.Cp - $cjkStart))
            $col++
            if ($col -ge 8) {
                [void]$sb.AppendLine($line.TrimEnd())
                $line = "    "
                $col = 0
            }
        }
        if ($col -gt 0) { [void]$sb.AppendLine($line.TrimEnd()) }
        [void]$sb.AppendLine("};")
        [void]$sb.AppendLine("")
    }
    [void]$sb.AppendLine("static const lv_font_fmt_txt_cmap_t cmaps[] = {")
    [void]$sb.AppendLine("    {")
    [void]$sb.AppendLine("        .range_start = 0x20, .range_length = 95, .glyph_id_start = 1,")
    [void]$sb.AppendLine("        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0,")
    [void]$sb.AppendLine("        .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY")
    [void]$sb.AppendLine("    },")
    if ($cjk.Count -gt 0) {
        $cjkEnd = $cjk[-1].Cp
        [void]$sb.AppendLine("    {")
        [void]$sb.AppendLine(("        .range_start = 0x{0:x}, .range_length = {1}, .glyph_id_start = {2}," -f `
            $cjkStart, ($cjkEnd - $cjkStart + 1), ($asciiCount + 1)))
        [void]$sb.AppendLine("        .unicode_list = unicode_list_1, .glyph_id_ofs_list = NULL,")
        [void]$sb.AppendLine(("        .list_length = {0}," -f $cjk.Count))
        [void]$sb.AppendLine("        .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY")
        [void]$sb.AppendLine("    }")
    }
    [void]$sb.AppendLine("};")
    $cmapNum = if ($cjk.Count -gt 0) { 2 } else { 1 }
    [void]$sb.AppendLine("")
    [void]$sb.AppendLine("static const lv_font_fmt_txt_dsc_t font_dsc = {")
    [void]$sb.AppendLine("    .glyph_bitmap = glyph_bitmap,")
    [void]$sb.AppendLine("    .glyph_dsc = glyph_dsc,")
    [void]$sb.AppendLine("    .cmaps = cmaps,")
    [void]$sb.AppendLine("    .kern_dsc = NULL,")
    [void]$sb.AppendLine("    .kern_scale = 0,")
    [void]$sb.AppendLine("    .cmap_num = {0}," -f $cmapNum)
    [void]$sb.AppendLine("    .bpp = 4,")
    [void]$sb.AppendLine("    .kern_classes = 0,")
    [void]$sb.AppendLine("    .bitmap_format = 0,")
    [void]$sb.AppendLine("    .stride = 0")
    [void]$sb.AppendLine("};")
    [void]$sb.AppendLine("")
    [void]$sb.AppendLine("const lv_font_t $name = {")
    [void]$sb.AppendLine("    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,")
    [void]$sb.AppendLine("    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,")
    [void]$sb.AppendLine("    .release_glyph = NULL,")
    [void]$sb.AppendLine(("    .line_height = {0}," -f $lineHeight))
    [void]$sb.AppendLine(("    .base_line = {0}," -f $baseLine))
    [void]$sb.AppendLine("    .subpx = LV_FONT_SUBPX_NONE,")
    [void]$sb.AppendLine("    .kerning = LV_FONT_KERNING_NORMAL,")
    [void]$sb.AppendLine("    .static_bitmap = 1,")
    [void]$sb.AppendLine("    .underline_position = -1,")
    [void]$sb.AppendLine("    .underline_thickness = 1,")
    [void]$sb.AppendLine("    .dsc = &font_dsc,")
    [void]$sb.AppendLine("    .fallback = NULL,")
    [void]$sb.AppendLine("    .user_data = NULL,")
    [void]$sb.AppendLine("};")
    [void]$sb.AppendLine("#endif /* $guard */")

    foreach ($dir in @($ScriptDir, $FontsDir)) {
        if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }
        $out = Join-Path $dir "$name.c"
        [System.IO.File]::WriteAllText($out, $sb.ToString(), [System.Text.UTF8Encoding]::new($false))
        Write-Host "wrote $out glyphs=$($glyphs.Count) bitmap_bytes=$($bitmapAll.Count) line_h=$lineHeight base=$baseLine"
    }
}

foreach ($s in $Sizes) { Emit-Font $s }
Write-Host "done"
