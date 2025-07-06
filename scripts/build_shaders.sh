#!/bin/bash

INPUT_DIR="../src/Graphics/Shaders"
OUTPUT_DIR="../res/shaders"

mkdir -p "$OUTPUT_DIR"

for shader in "$INPUT_DIR"/*.vert "$INPUT_DIR"/*.frag; do
    if [ ! -e "$shader" ]; then
        echo "No shader files found in $INPUT_DIR"
        exit 1
    fi

    filename=$(basename "$shader")
    name="${filename%.*}"

    glslc "$shader" -o "$OUTPUT_DIR/$name.spv"

    if [ $? -eq 0 ]; then
        echo "Compiled $shader to $OUTPUT_DIR/$name.spv"
    else
        echo "Failed to compile $shader"
    fi
done
