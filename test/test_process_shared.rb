# frozen_string_literal: true

require 'minitest/autorun'
require 'minitest/spec'
require 'process_shared'

describe ProcessShared do
  it 'works with threads' do
    mem = ProcessShared::MemorySegment.new
    monitor = ProcessShared::Monitor.new(mem)

    n_threads = 20
    iterations = 100
    target = iterations * n_threads
    counter = 0

    threads = n_threads.times.map do
      Thread.new do
        iterations.times do
          monitor.lock
          my_copy = counter
          sleep(rand(0..10000)/1000000.0)
          counter = my_copy + 1
          monitor.unlock
          sleep(rand(0..10000)/1000000.0)
        end
      end
    end
    threads.each(&:join)

    assert_equal target, counter
  end

  it 'works with processes' do
    mem = ProcessShared::MemorySegment.new
    monitor = ProcessShared::Monitor.new(mem)

    n_procs = 20
    iterations = 100

    rpipe, wpipe = IO.pipe

    pids = n_procs.times.map do
      fork do
        iterations.times do |i|
          monitor.lock
          wpipe.puts "LOCKED #{Process.pid}"
          sleep(rand(0..10000)/1000000.0)
          wpipe.puts "UNLOCKING #{Process.pid}"
          monitor.unlock
          sleep(rand(0..10000)/1000000.0)
        end
      end
    end
    wpipe.close
    lines = rpipe.read.lines.map(&:chomp)
    assert_equal(iterations * n_procs * 2, lines.length)
    # This is essentially Rails' in_groups_of(2)
    (0...(lines.length / 2)).each do |i|
      index = i*2
      assert_match (/^LOCKED ([0-9]+)$/), lines[index], "index=#{index}"
      pid = lines[index].split(' ')[1].to_i
      assert_kind_of Integer, pid
      assert_equal "UNLOCKING #{pid}", lines[index+1],  "index=#{index}"
    end
    rpipe.close
    pids.each { Process.waitpid2 _1 }
  end
end
