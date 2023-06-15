Gem::Specification.new do |s|
  s.name        = 'process_shared'
  s.version     = '0.0.1'
  s.summary     = 'Tools for coordinating across processes forked from a common parent'
  s.license     = 'Apache-2.0'
  s.authors     = ['KJ Tsanaktsidis', 'Daniel Menz']
  s.email       = ['ktsanaktsidis@zendesk.com', 'daniel.menz@zendesk.com']
  s.homepage    = 'https://github.com/zendesk/process_shared'

  s.extensions  = ['ext/process_shared_lock_ext/extconf.rb']
  s.files       = %w(
    ext/process_shared_ext/extconf.rb
    ext/process_shared_ext/process_shared_ext.c
    lib/process_shared.rb
  )

  s.add_development_dependency 'minitest', '~> 5.18'
  s.add_development_dependency 'extconf_compile_commands_json'
  s.add_development_dependency 'rake', '~> 13'
  s.add_development_dependency 'rake-compiler', '~> 1.2'
end
