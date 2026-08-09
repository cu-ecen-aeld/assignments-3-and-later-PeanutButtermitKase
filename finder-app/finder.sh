#!/bin/sh

# Check that exactly 2 arguments were provided
if [ "$#" -ne 2 ]; then
    # Print an error message if the arguments are missing
    echo "Error: Two arguments are required: filesdir and searchstr"
    # Exit with error code 1
    exit 1
fi

# Save the first argument as the directory path
filesdir="$1"

# Save the second argument as the search string
searchstr="$2"

# Verify that the provided path is a directory
if [ ! -d "$filesdir" ]; then
    # Print an error message if it is not a valid directory
    echo "Error: '$filesdir' is not a directory"
    # Exit with error code 1
    exit 1
fi

# Find all files recursively in the directory and count them
file_count=$(find "$filesdir" -type f | wc -l)

# Search recursively for matching lines and count them
match_count=$(grep -r "$searchstr" "$filesdir" 2>/dev/null | wc -l)

# Print the result in the exact format required by the assignment
echo "The number of files are $file_count and the number of matching lines are $match_count"