#!/bin/bash
set -euo pipefail

# filter.sh - Generate build matrix from build_list input
# Usage: filter.sh <build_list> <versions_dir>

BUILD_LIST="${1:-all}"
VERSIONS_DIR="${2:-.github/workflows/versions}"

# Function to parse .inf file and extract android version, kernel version
parse_version_from_filename() {
    local filename="$1"
    # Extract base name (e.g., "a14-6.1" from "a14-6.1.inf")
    basename "$filename" .inf
}

# Function to extract android_version from pattern (e.g., "android14" from "a14-6.1")
get_android_version() {
    local pattern="$1"
    echo "$pattern" | sed -E 's/^a([0-9]+)-.*/android\1/'
}

# Function to extract kernel_version from pattern (e.g., "6.1" from "a14-6.1")
get_kernel_version() {
    local pattern="$1"
    echo "$pattern" | sed -E 's/^a[0-9]+-([0-9]+)[.-]([0-9]+).*/\1.\2/'
}

# Function to check if a pattern exists in build list with exact boundary matching
pattern_exists() {
    local pattern="$1"
    local build_list="$2"
    
    # Check for exact match or with comma boundaries
    if [[ "$build_list" == "$pattern" ]] || \
       [[ "$build_list" == "$pattern,"* ]] || \
       [[ "$build_list" == *",$pattern,"* ]] || \
       [[ "$build_list" == *",$pattern" ]]; then
        return 0
    fi
    return 1
}

# Function to check if specific version exists in build list
version_exists() {
    local base_pattern="$1"
    local sub_level="$2"
    local build_list="$3"
    
    # Try different formats: a14-6.1-124, a14-6.1.124, a14-6-1-124
    local pattern_dash="${base_pattern}-${sub_level}"
    local pattern_dot="${base_pattern}.${sub_level}"
    local pattern_alt=$(echo "$base_pattern" | sed 's/\./-/g')"-${sub_level}"
    
    if pattern_exists "$pattern_dash" "$build_list" || \
       pattern_exists "$pattern_dot" "$build_list" || \
       pattern_exists "$pattern_alt" "$build_list"; then
        return 0
    fi
    return 1
}

# Initialize matrix output
matrix_json="[]"

# Process each .inf file
for inf_file in "$VERSIONS_DIR"/*.inf; do
    [ -f "$inf_file" ] || continue
    
    base_pattern=$(parse_version_from_filename "$inf_file")
    android_version=$(get_android_version "$base_pattern")
    kernel_version=$(get_kernel_version "$base_pattern")
    
    # Check if we should process this version file
    should_process=false
    
    # Check for "all" keyword
    if [[ "$BUILD_LIST" == *"all"* ]]; then
        should_process=true
    # Check if base pattern matches (with exact boundaries)
    elif pattern_exists "$base_pattern" "$BUILD_LIST" || \
         pattern_exists "$(echo "$base_pattern" | sed 's/\./-/g')" "$BUILD_LIST"; then
        should_process=true
    # Check for "lts" keyword
    elif [[ "$BUILD_LIST" == *"lts"* ]]; then
        # We'll filter LTS items while reading the file
        should_process=true
    # Check if any specific version from this file is requested
    else
        # Quick check if the build list contains any reference to this version
        if [[ "$BUILD_LIST" == *"$base_pattern"* ]]; then
            should_process=true
        fi
    fi
    
    if [ "$should_process" = false ]; then
        continue
    fi
    
    # Read the .inf file and process each line
    while IFS='=' read -r sub_level os_patch_level; do
        # Skip empty lines and section headers
        [[ -z "$sub_level" ]] && continue
        [[ "$sub_level" == \[*\] ]] && continue
        
        # Trim whitespace
        sub_level=$(echo "$sub_level" | xargs)
        os_patch_level=$(echo "$os_patch_level" | xargs)
        
        should_build=false
        
        # Check conditions for building this specific sublevel
        if [[ "$BUILD_LIST" == *"all"* ]]; then
            should_build=true
        elif [[ "$BUILD_LIST" == *"lts"* ]] && [[ "$sub_level" == "X" ]]; then
            should_build=true
        elif pattern_exists "$base_pattern" "$BUILD_LIST" || \
             pattern_exists "$(echo "$base_pattern" | sed 's/\./-/g')" "$BUILD_LIST"; then
            # Base pattern matched, build all sublevels
            should_build=true
        elif version_exists "$base_pattern" "$sub_level" "$BUILD_LIST"; then
            # Specific version matched
            should_build=true
        fi
        
        if [ "$should_build" = true ]; then
            # Add to matrix
            item=$(jq -n \
                --arg android "$android_version" \
                --arg kernel "$kernel_version" \
                --arg sub "$sub_level" \
                --arg patch "$os_patch_level" \
                '{android_version: $android, kernel_version: $kernel, sub_level: $sub, os_patch_level: $patch}')
            matrix_json=$(echo "$matrix_json" | jq ". += [$item]")
        fi
    done < "$inf_file"
done

# Output to GITHUB_OUTPUT if available, otherwise stdout
if [ -n "${GITHUB_OUTPUT:-}" ]; then
    echo "matrix=$(echo "$matrix_json" | jq -c '.')" >> "$GITHUB_OUTPUT"
    echo "count=$(echo "$matrix_json" | jq 'length')" >> "$GITHUB_OUTPUT"
else
    echo "matrix=$(echo "$matrix_json" | jq -c '.')"
    echo "count=$(echo "$matrix_json" | jq 'length')"
fi
