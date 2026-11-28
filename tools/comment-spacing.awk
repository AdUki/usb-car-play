# A blank line before a comment block of two or more lines that follows a statement or a
# closing brace, so the block reads as the start of what it describes. One-line comments (a
# member's note in a list of members) stay where they are. make format runs it before
# clang-format, which keeps the blank lines.
#
#   awk -f tools/comment-spacing.awk file.cpp > file.cpp.new

function flush(  i) {
	if (n >= 2 && after_code)
		print ""
	for (i = 1; i <= n; i++)
		print buf[i]
	n = 0
}

/^[ \t]*\/\// {
	if (n == 0)
		after_code = prev ~ /[;}][ \t]*$/ || prev ~ /^#[ \t]*(include|define|endif|pragma)/
	buf[++n] = $0
	next
}

{
	flush()
	print
	prev = $0
}

END { flush() }
