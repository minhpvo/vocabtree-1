FILE(REMOVE_RECURSE
  "CMakeFiles/python_cmd"
  "dummy_python_cmd"
)

# Per-language clean rules from dependency scanning.
FOREACH(lang)
  INCLUDE(CMakeFiles/python_cmd.dir/cmake_clean_${lang}.cmake OPTIONAL)
ENDFOREACH(lang)
