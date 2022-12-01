# cmake gives us a string with the literal ${EFFECTIVE_PLATFORM_NAME}, so replace it
LIBPATH=${1/'${EFFECTIVE_PLATFORM_NAME}'/${EFFECTIVE_PLATFORM_NAME}}

nm -gUj $LIBPATH | grep -v "^.*:$" | sort | uniq >> symbols.txt