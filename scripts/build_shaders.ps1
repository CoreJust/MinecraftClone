$INPUT_DIR = "src/Graphics/Shader"
$OUTPUT_DIR = "res/shaders"

if (-not (Test-Path -Path ./res)) {
    New-Item -ItemType Directory -Path ./res
}
if (-not (Test-Path -Path ./res/shaders)) {
    New-Item -ItemType Directory -Path ./res/shaders
}

$EXTENSIONS = @(".frag", ".vert")
$SHADERS = Get-ChildItem -Path $INPUT_DIR -File -Recurse | Where-Object { $_.Extension -in $EXTENSIONS }
foreach($shader_path in $SHADERS) {
    $shader = Split-Path -Path $shader_path -Leaf
    glslc.exe $shader_path -o $OUTPUT_DIR\\$shader.spv
    Write-Output Compiled shader $shader -> $OUTPUT_DIR\\$shader.spv
}
