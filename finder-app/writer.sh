#!/bin/sh

# Check that exactly 2 arguments were provided
if [ "$#" -ne 2 ]; then
    echo "Error: Two arguments are required: writefile and writestr"
    exit 1
fi

# Store arguments in variables
writefile="$1"
writestr="$2"

# Extract the directory path from the full file path
writedir=$(dirname "$writefile")

# Create the directory path if it does not already exist
mkdir -p "$writedir"

# Check if directory creation failed
if [ $? -ne 0 ]; then
    echo "Error: Could not create directory path '$writedir'"
    exit 1
fi

# Write the string into the file, overwriting any existing content
echo "$writestr" > "$writefile"

# Check if file creation or writing failed
if [ $? -ne 0 ]; then
    echo "Error: Could not create or write to file '$writefile'"
    exit 1
fi