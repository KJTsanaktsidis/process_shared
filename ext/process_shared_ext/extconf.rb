# frozen_string_literal: true

require 'mkmf'
require 'extconf_compile_commands_json'

create_makefile 'process_shared_ext'

ExtconfCompileCommandsJson.generate!
ExtconfCompileCommandsJson.symlink!
