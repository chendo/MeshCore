#!/usr/bin/env ruby
# frozen_string_literal: true
#
# meshllm -- answer "@" messages from a MeshCore room and DMs with a local LLM.
#
#   room post / DM starting with "@"  ->  llmswarm (OpenAI-compatible)
#   ->  flattened, truncated to the 140-byte mesh limit  ->  posted back
#
# It runs on the host and drives the multi-identity node's *companion* identity
# through the web panel's frame mux (POST /api/multi/comp/frame), so it shares
# the companion with a live phone app instead of kicking it off the way a raw
# TCP :5000 client would.
#
# Two inputs, both gated on the "@" prefix:
#   * room  -- new posts, read from GET /api/multi/room/posts (the local room's
#              cyclic queue, no login needed) and/or from room pushes that
#              arrive at the companion once it is logged in. Replies are posted
#              into the room as a room client, so everyone logged in sees them.
#   * DMs   -- contact messages to the companion; replies go back as DMs.
#
# Trust boundary worth knowing: a room post's author is a pubkey prefix (real
# identity), but anyone who can log into the room can trigger the bot. Rate
# limits are per author prefix.
#
# Usage:
#   ./meshllm.rb run                  bridge room + DMs (main mode)
#   ./meshllm.rb probe                connect, show identities/room/limits/LLM
#   ./meshllm.rb pair-room            add this box's own room as a contact,
#                                     reading its key off the device (no advert)
#   ./meshllm.rb say "text"           post into the bridged room (live check)
#   ./meshllm.rb console "room ..."   run a panel console command
#   ./meshllm.rb ask "question"       LLM + shaping only, no radio
#   ./meshllm.rb shape "long text"    preview the 140-byte shaping
#   ./meshllm.rb selftest             frame codec + shaping checks, offline
#
# Config: ./config.json or ~/.meshllm/config.json (see config.example.json).
# MESHLLM_HOST / MESHLLM_PASSWORD override the node address and admin password.

require 'digest'
require 'fileutils'
require 'json'
require 'net/http'
require 'openssl'
require 'optparse'
require 'thread'
require 'time'
require 'uri'

module MeshLLM
  VERSION = '1.0'

  DEFAULTS = {
    'node' => { 'host' => '', 'port' => 443, 'password' => 'password', 'verify_tls' => false },
    'room' => { 'enabled' => true, 'name' => '', 'source' => 'auto', 'password' => '',
                'reply' => 'post', 'relogin_minutes' => 60 },
    'dm' => { 'enabled' => true },
    'trigger' => '@',
    'bot_name' => '',
    'reply_limit' => 140,
    'max_chunks' => 1,
    'chunk_gap_seconds' => 5,
    'poll_seconds' => 4,
    'contacts_refresh_minutes' => 15,
    'state_file' => '~/.meshllm/state.json',
    'llm' => {
      'url' => 'http://localhost:50080/v1/chat/completions',
      'model' => 'Qwen3.6-35B-A3B-Q8_0',
      'api_key' => '',
      'max_tokens' => 96,
      'temperature' => 0.3,
      'timeout_seconds' => 60,
      'thinking' => false,
      'history_turns' => 6,
      'history_idle_minutes' => 30,
      'system_prompt' => nil
    },
    'limits' => { 'per_sender_seconds' => 20, 'per_sender_hourly' => 20,
                  'global_hourly' => 60, 'queue_depth' => 2, 'prompt_chars' => 400 }
  }.freeze

  # Qwen3.6 is a reasoning model: left to itself it spends the whole token
  # budget in reasoning_content and returns empty content. The brevity contract
  # here is belt; Shaper.chunk is braces.
  SYSTEM_PROMPT = <<~PROMPT.gsub("\n", ' ').strip
    You are a helpful assistant reachable over a LoRa mesh radio. Every reply is
    hard-limited to %<limit>d characters, so answer in ONE short sentence.
    Plain text only: no markdown, no bullet points, no headings, no code fences,
    no preamble, no sign-off, no restating the question. Aim for under
    %<soft>d characters. If the full answer cannot fit, give the single most
    useful fact instead of a truncated essay. If you do not know or would need
    live data you do not have, say so in a few words.
  PROMPT

  HELP = '@<question> to ask. @!help @!status @!reset. Replies are radio-sized.'

  class Error < StandardError; end

  # A mistake in the config (unknown room, nothing to bridge). Unlike a network
  # fault this will never fix itself, so startup gives up instead of retrying.
  class ConfigError < Error; end

  def self.log(msg)
    warn "#{Time.now.strftime('%H:%M:%S')} #{msg}"
  end

  # ---------------------------------------------------------------- protocol

  # Companion app-protocol frames. See docs/companion_protocol.md and
  # examples/companion_radio/MyMesh.cpp for the authoritative layouts.
  module Proto
    CMD_APP_START            = 1
    CMD_SEND_TXT_MSG         = 2
    CMD_GET_CONTACTS         = 4
    CMD_ADD_UPDATE_CONTACT   = 9
    CMD_SYNC_NEXT_MESSAGE    = 10
    CMD_DEVICE_QUERY         = 22
    CMD_SEND_LOGIN           = 26

    RESP_OK                  = 0
    RESP_ERR                 = 1
    RESP_CONTACT             = 3
    RESP_SELF_INFO           = 5
    RESP_SENT                = 6
    RESP_CONTACT_MSG         = 7
    RESP_NO_MORE_MSGS        = 10
    RESP_CONTACT_MSG_V3      = 16
    PUSH_SEND_CONFIRMED      = 0x82
    PUSH_LOGIN_SUCCESS       = 0x85
    PUSH_LOGIN_FAIL          = 0x86

    ADV_TYPE_ROOM            = 3
    TXT_TYPE_PLAIN           = 0
    TXT_TYPE_SIGNED_PLAIN    = 2
    OUT_PATH_UNKNOWN         = 0xFF
    MAX_PATH_SIZE            = 64

    # BaseChatMesh::MAX_TEXT_LEN (10 * CIPHER_BLOCK_SIZE); room posts are
    # additionally clamped to MAX_POST_TEXT_LEN = 160-9 = 151 by the room.
    MAX_TEXT_LEN  = 160
    MAX_POST_TEXT = 151

    module_function

    def u32(str, off)
      str.byteslice(off, 4).to_s.unpack1('V') || 0
    end

    def i8(byte)
      byte > 127 ? byte - 256 : byte
    end

    def cstr(str, off, len)
      raw = str.byteslice(off, len).to_s
      raw = raw.byteslice(0, raw.index("\x00") || raw.bytesize)
      raw.to_s.dup.force_encoding(Encoding::UTF_8).scrub('')
    end

    def hex(str, off, len)
      str.byteslice(off, len).to_s.unpack1('H*') || ''
    end

    # POST /comp/frame body: [u16 len][frame]...
    def split_frames(blob)
      out = []
      off = 0
      while off + 2 <= blob.bytesize
        len = blob.getbyte(off) | (blob.getbyte(off + 1) << 8)
        out << blob.byteslice(off + 2, len).to_s
        off += 2 + len
      end
      out
    end

    # GET /comp/archive body: [u32 latest][u32 seq][u16 len][frame]...
    def parse_archive(blob)
      return [0, []] if blob.bytesize < 4

      latest = u32(blob, 0)
      out = []
      off = 4
      while off + 6 <= blob.bytesize
        seq = u32(blob, off)
        len = blob.getbyte(off + 4) | (blob.getbyte(off + 5) << 8)
        frame = blob.byteslice(off + 6, len).to_s
        break if frame.bytesize < len

        out << [seq, frame]
        off += 6 + len
      end
      [latest, out]
    end

    # Decode a contact message frame (plain DM, or a room post pushed to us).
    # Room pushes are TXT_TYPE_SIGNED_PLAIN with the post author's 4-byte
    # pubkey prefix sitting in the signature field (see
    # examples/simple_room_server/MyMesh.cpp:pushPostToClient).
    def parse_contact_msg(frame)
      code = frame.getbyte(0)
      return nil unless [RESP_CONTACT_MSG, RESP_CONTACT_MSG_V3].include?(code)

      off = code == RESP_CONTACT_MSG_V3 ? 4 : 1
      snr = code == RESP_CONTACT_MSG_V3 ? i8(frame.getbyte(1)) / 4.0 : nil
      return nil if frame.bytesize < off + 12

      from      = hex(frame, off, 6)
      txt_type  = frame.getbyte(off + 7)
      timestamp = u32(frame, off + 8)
      body      = off + 12
      author    = nil
      if txt_type == TXT_TYPE_SIGNED_PLAIN
        author = hex(frame, body, 4)
        body += 4
      end
      text = frame.byteslice(body..-1).to_s.dup.force_encoding(Encoding::UTF_8).scrub('')
      { from: from, author: author, txt_type: txt_type, timestamp: timestamp,
        snr: snr, text: text }
    end

    def app_start(name)
      [CMD_APP_START, 0, 0, 0, 0, 0, 0, 0].pack('C8') + name.byteslice(0, 16)
    end

    def send_txt(pubkey_prefix_hex, text, timestamp = Time.now.to_i)
      [CMD_SEND_TXT_MSG, TXT_TYPE_PLAIN, 0].pack('C3') +
        [timestamp].pack('V') +
        [pubkey_prefix_hex.byteslice(0, 12)].pack('H*') +
        text.dup.force_encoding(Encoding::BINARY)
    end

    # Add a contact without waiting for an advert -- lets us pair with this
    # box's own room without asking a deliberately private room to announce
    # itself. Layout per MyMesh::updateContactFromFrame.
    def add_contact(pubkey_hex, name, type)
      [CMD_ADD_UPDATE_CONTACT].pack('C') +
        [pubkey_hex].pack('H*') +
        [type, 0, OUT_PATH_UNKNOWN].pack('C3') +      # type, flags, no known path
        ("\x00" * MAX_PATH_SIZE) +
        name.to_s.byteslice(0, 31).to_s.ljust(32, "\x00") +
        [Time.now.to_i].pack('V')                     # last_advert_timestamp
    end

    def send_login(pubkey_hex, password)
      [CMD_SEND_LOGIN].pack('C') + [pubkey_hex.byteslice(0, 64)].pack('H*') +
        password.to_s.dup.force_encoding(Encoding::BINARY)
    end
  end

  # ------------------------------------------------------------ panel client

  # Keep-alive HTTPS client for the node's panel API. The panel keeps only a
  # handful of sockets and 4 session tokens, so we hold one connection and one
  # token, re-logging in only when it is evicted (401).
  class PanelClient
    def initialize(cfg)
      @host = cfg['host'].to_s
      @port = cfg['port'] || 443
      @password = cfg['password'].to_s
      @verify = cfg['verify_tls'] ? OpenSSL::SSL::VERIFY_PEER : OpenSSL::SSL::VERIFY_NONE
      raise Error, 'node.host is not set (config or MESHLLM_HOST)' if @host.empty?

      @token = nil
      @mutex = Mutex.new
    end

    def http
      if @http.nil? || !@http.started?
        @http = Net::HTTP.new(@host, @port)
        @http.use_ssl = true
        @http.verify_mode = @verify
        @http.open_timeout = 10
        @http.read_timeout = 25
        @http.keep_alive_timeout = 30
        @http.start
      end
      @http
    end

    def close
      @http.finish if @http && @http.started?
    rescue StandardError
      nil
    ensure
      @http = nil
    end

    def login
      req = Net::HTTP::Post.new('/login')
      req['Content-Type'] = 'text/plain'
      req.body = @password
      res = perform(req)
      raise Error, "login rejected (HTTP #{res.code}) -- wrong admin password?" unless res.code == '200'

      @token = res.body.to_s.strip
    end

    # Serialized: the mux handles one web frame exchange at a time anyway.
    def request(req)
      @mutex.synchronize do
        login if @token.nil?
        req['X-Auth-Token'] = @token
        res = perform(req)
        if res.code == '401'                     # token evicted by another login
          login
          req['X-Auth-Token'] = @token
          res = perform(req)
        end
        raise Error, "#{req.path} -> HTTP #{res.code}" unless res.code == '200'

        res.body.to_s
      end
    end

    def frame(payload, total_ms: 2500, idle_ms: 300)
      req = Net::HTTP::Post.new("/api/multi/comp/frame?t=#{total_ms}&i=#{idle_ms}")
      req['Content-Type'] = 'application/octet-stream'
      req.body = payload.dup.force_encoding(Encoding::BINARY)
      Proto.split_frames(binary(request(req)))
    end

    def archive(after)
      Proto.parse_archive(binary(request(Net::HTTP::Get.new("/api/multi/comp/archive?after=#{after}"))))
    end

    def debug
      JSON.parse(request(Net::HTTP::Get.new('/api/multi/debug')))
    rescue JSON::ParserError
      {}
    end

    def room_posts
      JSON.parse(request(Net::HTTP::Get.new('/api/multi/room/posts')))
    rescue JSON::ParserError
      []
    end

    # The panel's console: `room advert`, `identities`, ...
    def console(command)
      req = Net::HTTP::Post.new('/api/command')
      req['Content-Type'] = 'text/plain'
      req.body = command
      request(req)
    end

    private

    def binary(str)
      str.dup.force_encoding(Encoding::BINARY)
    end

    def perform(req)
      attempts = 0
      begin
        attempts += 1
        http.request(req)
      rescue StandardError => e
        close
        raise Error, "#{req.method} #{req.path}: #{e.class}: #{e.message}" if attempts > 1

        retry
      end
    end
  end

  # ------------------------------------------------------------- companion

  Contact = Struct.new(:pubkey, :prefix, :type, :name, keyword_init: true)

  class Companion
    attr_reader :self_name, :self_pubkey, :contacts

    def initialize(client, app_name: 'meshllm')
      @client = client
      @app_name = app_name
      @self_name = ''
      @self_pubkey = ''
      @contacts = []
    end

    # The mux serves one exchange at a time and the phone app's own syncs can
    # hold it for seconds (a 160-contact sync is a lot of frames), so a single
    # empty answer means "busy", not "broken". Retry with a longer window.
    def init(attempts: 3)
      attempts.times do |i|
        window = 2500 + (i * 3000)
        @client.frame([Proto::CMD_DEVICE_QUERY, 3].pack('C2'), total_ms: window)
        @client.frame(Proto.app_start(@app_name), total_ms: window, idle_ms: 400).each do |f|
          next unless f.getbyte(0) == Proto::RESP_SELF_INFO && f.bytesize > 58

          # SELF_INFO: [4..35] pubkey, [58..] name (MyMesh.cpp:1121)
          @self_pubkey = Proto.hex(f, 4, 32)
          @self_name = Proto.cstr(f, 58, f.bytesize - 58)
        end
        return self unless @self_pubkey.empty?

        sleep 2
      end
      raise Error, 'companion did not answer APP_START (device busy with the phone app?)'
    end

    def load_contacts(attempts: 3)
      attempts.times do |i|
        found = @client.frame([Proto::CMD_GET_CONTACTS].pack('C'),
                              total_ms: 6000 + (i * 2000), idle_ms: 600)
                       .select { |f| f.getbyte(0) == Proto::RESP_CONTACT && f.bytesize >= 148 }
                       .map do |f|
          Contact.new(pubkey: Proto.hex(f, 1, 32), prefix: Proto.hex(f, 1, 6),
                      type: f.getbyte(33), name: Proto.cstr(f, 100, 32))
        end
        # A short answer means the sync got cut off by contention, not that
        # contacts vanished -- with 150+ contacts the reply takes a while.
        truncated = found.length < @contacts.length / 2
        if (found.empty? || truncated) && i < attempts - 1
          sleep 2
          next
        end
        return @contacts = found if found.length >= @contacts.length

        @contacts = found unless found.empty? || truncated
        return @contacts
      end
      @contacts
    end

    def rooms
      @contacts.select { |c| c.type == Proto::ADV_TYPE_ROOM }
    end

    def name_for(prefix)
      return @self_name if !prefix.to_s.empty? && @self_pubkey.start_with?(prefix.to_s)

      c = @contacts.find { |x| x.prefix.start_with?(prefix.to_s) || prefix.to_s.start_with?(x.prefix) }
      c ? c.name : prefix.to_s[0, 8]
    end

    def phone_attached?
      !!@client.debug.dig('companion', 'client')
    end

    # Log in to a room server so it pushes new posts to us and accepts ours.
    # The room grants PERM_ACL_READ_WRITE for the room ("guest") password and
    # read-only for a wrong one, so a silent no-post usually means a bad
    # password (simple_room_server/MyMesh.cpp:onAnonDataRecv).
    def login_room(room, password)
      @client.frame(Proto.send_login(room.pubkey, password), total_ms: 4000, idle_ms: 400)
             .any? { |f| f.getbyte(0) == Proto::RESP_SENT }
    end

    def send_dm(prefix, text)
      @client.frame(Proto.send_txt(prefix, text), total_ms: 5000, idle_ms: 400)
             .each { |f| return true if f.getbyte(0) == Proto::RESP_SENT }
      false
    end

    def add_contact(pubkey_hex, name, type)
      @client.frame(Proto.add_contact(pubkey_hex, name, type), total_ms: 4000, idle_ms: 400)
             .any? { |f| f.getbyte(0) == Proto::RESP_OK }
    end

    # Drain the device's offline queue. Destructive -- a phone app would never
    # see what we pull -- so callers only do this when no phone is attached.
    def drain(limit: 16)
      out = []
      limit.times do
        frames = @client.frame([Proto::CMD_SYNC_NEXT_MESSAGE].pack('C'))
        f = frames.first
        break if f.nil? || f.getbyte(0) == Proto::RESP_NO_MORE_MSGS

        msg = Proto.parse_contact_msg(f)
        out << msg if msg
      end
      out
    end

    # Mirrored frames: messages plus every async push. Returns [new_seq, msgs,
    # pushes]. The archive is RAM-only, so latest < after means a reboot.
    def poll_archive(after)
      latest, entries = @client.archive(after)
      return [latest, [], []] if latest < after

      seq = after
      msgs = []
      pushes = []
      entries.each do |(s, frame)|
        next if s <= seq

        seq = s
        code = frame.getbyte(0)
        if code && code >= 0x80
          pushes << code
          next
        end
        msg = Proto.parse_contact_msg(frame)
        if msg
          msg[:seq] = s
          msgs << msg
        end
      end
      [[seq, latest].max, msgs, pushes]
    end
  end

  # ------------------------------------------------------------------- LLM

  class Llm
    def initialize(cfg)
      @cfg = cfg
      @uri = URI.parse(cfg['url'])
      @http = nil
    end

    def health
      base = @uri.dup
      base.path = '/health'
      base.query = nil
      res = Net::HTTP.get_response(base)
      JSON.parse(res.body)
    rescue StandardError => e
      { 'status' => "unreachable (#{e.class})" }
    end

    def models
      base = @uri.dup
      base.path = '/v1/models'
      base.query = nil
      JSON.parse(Net::HTTP.get_response(base).body).fetch('data', []).map { |m| m['id'] }
    rescue StandardError
      []
    end

    # messages: [{role:, content:}]. Returns the reply text.
    def chat(messages)
      body = {
        'model' => @cfg['model'],
        'messages' => messages,
        'max_tokens' => @cfg['max_tokens'],
        'temperature' => @cfg['temperature'],
        'stream' => false
      }
      # llama.cpp honours the chat template's enable_thinking switch; without
      # it Qwen3.6 emits reasoning_content and leaves content empty.
      body['chat_template_kwargs'] = { 'enable_thinking' => !!@cfg['thinking'] }

      req = Net::HTTP::Post.new(@uri.request_uri)
      req['Content-Type'] = 'application/json'
      key = @cfg['api_key'].to_s
      req['Authorization'] = "Bearer #{key}" unless key.empty?
      req.body = JSON.generate(body)

      http = Net::HTTP.new(@uri.host, @uri.port)
      http.use_ssl = @uri.scheme == 'https'
      http.open_timeout = 10
      http.read_timeout = @cfg['timeout_seconds'] || 60
      res = http.request(req)
      # A backend that is reloading or wedged answers 5xx; one retry costs
      # little and covers the common transient case.
      if res.code.to_i >= 500
        sleep 2
        res = http.request(req)
      end
      raise Error, "llm HTTP #{res.code}: #{res.body.to_s[0, 160]}" unless res.code == '200'

      data = JSON.parse(res.body)
      raise Error, "llm error: #{data['error']}" if data['error']

      msg = data.dig('choices', 0, 'message') || {}
      text = msg['content'].to_s
      if text.strip.empty?
        # Thinking leaked into reasoning_content and ate the budget: salvage a
        # sentence from it rather than replying with nothing.
        reasoned = msg['reasoning_content'].to_s
        raise Error, 'model returned no answer (thinking used the whole budget)' if reasoned.strip.empty?

        text = reasoned.split(/(?<=[.!?])\s+/).last(1).join(' ')
      end
      text
    rescue JSON::ParserError => e
      raise Error, "llm bad JSON: #{e.message}"
    rescue Errno::ECONNREFUSED
      raise Error, "llm unreachable at #{@uri.host}:#{@uri.port}"
    rescue Net::ReadTimeout, Net::OpenTimeout
      raise Error, "llm timed out after #{@cfg['timeout_seconds']}s"
    end
  end

  # --------------------------------------------------------------- shaping

  module Shaper
    module_function

    # Markdown and newlines to one plain line.
    def flatten(text)
      t = text.to_s.dup
      t.gsub!(/```.*?```/m, ' ')
      t.gsub!(/`([^`]*)`/, '\1')
      t.gsub!(/\[([^\]]+)\]\(([^)]+)\)/, '\1 \2')
      t.gsub!(/^\s*\#{1,6}\s*/, '')
      t.gsub!(/^\s*(?:[-*+]|\d+[.)])\s+/, '; ')
      t.gsub!(/(\*\*|__)(.+?)\1/m, '\2')
      t.gsub!(/(?<![a-zA-Z0-9])[*_](\S.*?\S)[*_](?![a-zA-Z0-9])/m, '\1')
      t.gsub!(/<\/?think>/, ' ')
      t.tr!("\r", ' ')
      t.gsub!(/\n{2,}/, ' ; ')
      t.tr!("\n", ' ')
      t.gsub!(/\s{2,}/, ' ')
      t.gsub!(/(?:\s*;\s*)+/, '; ')            # ". ; item" -> "; item", saves bytes
      t.strip.sub(/\A;\s*/, '').strip
    end

    # Longest prefix of text within limit bytes, cut on a character boundary.
    def fit(text, limit)
      return text if text.bytesize <= limit

      text.byteslice(0, limit).to_s.scrub('')
    end

    def clip(text, limit)
      return text if text.bytesize <= limit

      head = fit(text, limit - 3)
      cut = head.rindex(' ')
      head = head[0, cut] if cut && cut > head.length / 2
      "#{head.rstrip}..."
    end

    # One message by default; more only if max_chunks allows it.
    def chunk(text, limit, max_chunks = 1)
      flat = flatten(text)
      return [] if flat.empty?
      return [flat] if flat.bytesize <= limit
      return [clip(flat, limit)] if max_chunks <= 1

      budget = limit - 4                        # room for a "1/2 " marker
      parts = []
      rest = flat
      while !rest.empty? && parts.length < max_chunks
        last = parts.length == max_chunks - 1
        if rest.bytesize <= budget
          parts << rest
          break
        end
        if last
          parts << clip(rest, budget)
          break
        end
        head = fit(rest, budget)
        cut = head.rindex(' ')
        head = head[0, cut] if cut && cut > head.length / 2
        parts << head.rstrip
        rest = rest[head.length..-1].to_s.lstrip
      end
      return [clip(parts.first, limit)] if parts.length == 1

      parts.each_with_index.map { |p, i| "#{i + 1}/#{parts.length} #{p}" }
    end
  end

  # ----------------------------------------------------------------- state

  # Everything that must survive a restart: the archive cursor, the newest room
  # post we have answered, a dedup ring, and per-conversation history.
  class State
    def initialize(path, idle_minutes)
      @path = File.expand_path(path)
      @idle = idle_minutes.to_i * 60
      @data = { 'arch_seq' => 0, 'last_post_ts' => 0, 'seen' => [], 'history' => {} }
      if File.exist?(@path)
        begin
          loaded = JSON.parse(File.read(@path))
          @data.merge!(loaded) if loaded.is_a?(Hash)
        rescue JSON::ParserError
          MeshLLM.log "state file unreadable, starting fresh: #{@path}"
        end
      end
    end

    def [](key)
      @data[key]
    end

    def []=(key, value)
      @data[key] = value
    end

    def save
      FileUtils.mkdir_p(File.dirname(@path))
      tmp = "#{@path}.tmp"
      File.write(tmp, JSON.pretty_generate(@data))
      File.rename(tmp, @path)
    rescue SystemCallError => e
      MeshLLM.log "could not save state: #{e.message}"
    end

    def seen?(key)
      @data['seen'].include?(key)
    end

    def mark_seen(key, keep = 300)
      @data['seen'] << key
      @data['seen'] = @data['seen'].last(keep)
    end

    def history(scope, turns)
      h = @data['history'][scope]
      return [] if h.nil?
      if @idle.positive? && Time.now.to_i - h['last'].to_i > @idle
        @data['history'].delete(scope)
        return []
      end
      h['messages'].to_a.last(turns * 2)
    end

    def remember(scope, user, assistant, turns)
      h = @data['history'][scope] ||= { 'messages' => [] }
      h['messages'] = (h['messages'] + [{ 'role' => 'user', 'content' => user },
                                        { 'role' => 'assistant', 'content' => assistant }])
                      .last(turns * 2)
      h['last'] = Time.now.to_i
    end

    def reset(scope)
      !@data['history'].delete(scope).nil?
    end
  end

  # ----------------------------------------------------------------- limits

  class Limiter
    def initialize(cfg)
      @per_seconds = cfg['per_sender_seconds'].to_i
      @per_hour = cfg['per_sender_hourly'].to_i
      @global_hour = cfg['global_hourly'].to_i
      @last = {}
      @hits = {}
      @all = []
    end

    # nil when allowed, else a short reason (safe to say out loud).
    def check(sender)
      now = Time.now.to_f
      hour = now - 3600

      # Silent check first: someone hammering the bot is already over their
      # hourly cap, and answering "you are over your cap" to every message
      # would put us on the air as often as they are.
      wait = @per_seconds - (now - (@last[sender] || 0))
      return "wait #{wait.ceil}s" if wait.positive?

      @all.reject! { |t| t < hour }
      return 'hourly limit for this bot reached' if @all.length >= @global_hour

      hits = (@hits[sender] ||= [])
      hits.reject! { |t| t < hour }
      return 'your hourly limit reached' if hits.length >= @per_hour

      nil
    end

    def note(sender)
      now = Time.now.to_f
      @last[sender] = now
      (@hits[sender] ||= []) << now
      @all << now
    end
  end

  # ----------------------------------------------------------------- bridge

  Job = Struct.new(:scope, :prompt, :sender, :reply, keyword_init: true)

  class Bridge
    def initialize(cfg, verbose: false)
      @cfg = cfg
      @verbose = verbose
      @trigger = cfg['trigger'].to_s.empty? ? '@' : cfg['trigger'].to_s
      @bot_name = cfg['bot_name'].to_s.strip.downcase
      @limit = cfg['reply_limit'].to_i
      @chunks = [cfg['max_chunks'].to_i, 1].max
      @state = State.new(cfg['state_file'], cfg['llm']['history_idle_minutes'])
      @limiter = Limiter.new(cfg['limits'])
      @llm = Llm.new(cfg['llm'])
      @client = PanelClient.new(cfg['node'])
      @comp = Companion.new(@client)
      @queue = Queue.new
      @stop = false
      @started = Time.now
      @answered = 0
      @room = nil
      @room_local = false
      @room_src = nil
      @local_room_prefix = ''
      @next_login = 0.0
    end

    def stop!
      @stop = true
    end

    # ---------------------------------------------------------- connection

    def connect
      @comp.init
      MeshLLM.log "companion: #{@comp.self_name.empty? ? '?' : @comp.self_name} (#{@comp.self_pubkey[0, 12]})"
      @comp.load_contacts
      MeshLLM.log "contacts: #{@comp.contacts.length}"

      if @cfg['room']['enabled']
        @local_room_prefix = local_room_prefix
        @room = select_room
        if @room.nil?
          MeshLLM.log 'room: disabled (no room contact selected)'
        else
          # Reading a room means one of two different things depending on which
          # room it is: this box's own room identity stores its posts locally
          # (readable over HTTP, no login), a remote room pushes them to logged-in
          # clients over RF. Resolving it here keeps the two from crossing.
          @room_local = !@local_room_prefix.empty? && @room.prefix.start_with?(@local_room_prefix)
          @room_src = resolve_room_source
          MeshLLM.log "room: #{@room.name} (#{@room.prefix[0, 12]}) " \
                      "#{@room_local ? 'local identity' : 'remote'}, reading via #{@room_src}"
          room_login
        end
      end
      # A room post is clamped to MAX_POST_TEXT (151) and a DM to MAX_TEXT_LEN
      # (160); 140 keeps us inside both with margin.
      @limit = [@limit, Proto::MAX_POST_TEXT].min
      sources = []
      sources << "room #{@room.name}" if @room
      sources << 'DMs' if @cfg['dm']['enabled']
      raise ConfigError, 'nothing to bridge: no room selected and dm.enabled is false' if sources.empty?

      MeshLLM.log "bridging #{sources.join(' + ')} -> #{@cfg['llm']['model']}, " \
                  "replies <=#{@limit} bytes x#{@chunks}"
    end

    # The node reboots, WiFi drops, the daemon outlives both. Only a config
    # mistake is fatal at startup; anything else is worth waiting out.
    def connect_with_retry
      delay = 5
      begin
        connect
      rescue ConfigError
        raise
      rescue Error, StandardError => e
        return if @stop

        MeshLLM.log "!! startup: #{e.message} -- retrying in #{delay}s"
        sleep delay
        delay = [delay * 2, 120].min
        retry unless @stop
      end
    end

    # "room=c1a551f1" out of the console's identities line.
    def local_room_prefix
      @client.console('identities').to_s[/room=([0-9a-fA-F]+)/, 1].to_s.downcase
    rescue Error => e
      MeshLLM.log "room: could not read identities (#{e.message})"
      ''
    end

    # Rooms are chosen by name: there are usually several in a companion's
    # contacts and answering in the wrong one is worse than not starting.
    def select_room
      rooms = @comp.rooms
      want = @cfg['room']['name'].to_s.strip
      if rooms.empty?
        MeshLLM.log 'room: no ADV_TYPE_ROOM contact -- run `meshllm.rb pair-room` for ' \
                    'this box\'s own room, or wait for the room you want to advert'
        return nil
      end
      unless want.empty?
        # A pubkey prefix wins over a name: two rooms can share a name (this
        # box's own room and a remote one, for instance).
        if want =~ /\A[0-9a-fA-F]{4,}\z/
          hit = rooms.select { |r| r.prefix.start_with?(want.downcase) }
          return hit.first if hit.length == 1
        end
        hits = rooms.select { |r| r.name.strip.casecmp(want).zero? }
        return hits.first if hits.length == 1
        if hits.length > 1
          raise ConfigError, "#{hits.length} rooms are named #{want.inspect}; set room.name to a " \
                       "pubkey prefix instead: #{hits.map { |r| r.prefix[0, 12] }.join(', ')}"
        end

        raise ConfigError, "room #{want.inspect} is not a contact. Known rooms: " +
                     rooms.map { |r| "#{r.name} (#{r.prefix[0, 12]})" }.join(', ')
      end
      return rooms.first if rooms.length == 1

      raise ConfigError, 'set room.name -- this companion knows several rooms: ' +
                   rooms.map { |r| "#{r.name} (#{r.prefix[0, 12]})" }.join(', ')
    end

    def resolve_room_source
      src = @cfg['room']['source'].to_s
      return src unless src == 'auto'

      @room_local ? 'posts' : 'push'
    end

    def room_login
      return false if @room.nil?

      ok = @comp.login_room(@room, @cfg['room']['password'].to_s)
      @next_login = Time.now.to_f + (@cfg['room']['relogin_minutes'].to_i * 60)
      MeshLLM.log(ok ? 'room: login sent' : 'room: login command rejected by device')
      ok
    rescue Error => e
      MeshLLM.log "room: login failed: #{e.message}"
      false
    end

    # -------------------------------------------------------------- intake

    # Returns the prompt when a message is addressed to us, else nil.
    def match_trigger(text)
      t = text.to_s.strip
      return nil unless t.start_with?(@trigger)

      body = t[@trigger.length..-1].to_s.strip
      unless @bot_name.empty?
        first, rest = body.split(/\s+/, 2)
        body = rest.to_s.strip if first && first.downcase.delete(':,') == @bot_name
      end
      body
    end

    def enqueue(prompt:, sender:, scope:, reply:)
      if prompt.start_with?('!')
        text = builtin(prompt[1..-1].to_s.strip.downcase, scope)
        send_parts(reply, [text]) if text
        return
      end
      if prompt.empty?
        send_parts(reply, [Shaper.clip(HELP, @limit)])
        return
      end

      cap = @cfg['limits']['prompt_chars'].to_i
      prompt = prompt[0, cap] if cap.positive? && prompt.length > cap

      why = @limiter.check(sender)
      if why
        MeshLLM.log "   rate-limited #{sender}: #{why}"
        # Say something for a real cap, stay quiet for the anti-spam interval.
        send_parts(reply, [why]) unless why.start_with?('wait')
        return
      end
      depth = @cfg['limits']['queue_depth'].to_i
      if @queue.size >= depth
        send_parts(reply, ["busy (#{@queue.size} queued), try again shortly"])
        return
      end
      @limiter.note(sender)
      @queue << Job.new(scope: scope, prompt: prompt, sender: sender, reply: reply)
    end

    def builtin(cmd, scope)
      case cmd
      when 'help', '?', '' then Shaper.clip(HELP, @limit)
      when 'reset'
        gone = @state.reset(scope)
        @state.save
        gone ? 'forgotten, fresh chat' : 'already a fresh chat'
      when 'status'
        up = (Time.now - @started).to_i
        format('up %dh%02dm, q%d, answered %d, %s', up / 3600, (up % 3600) / 60,
               @queue.size, @answered, @cfg['llm']['model'])
      else "unknown -- #{Shaper.clip(HELP, @limit - 12)}"
      end
    end

    # -------------------------------------------------------------- sources

    # New room posts from the panel endpoint: [{t:, a:<author prefix>, x:<text>}]
    def poll_room_posts
      # The posts endpoint only ever shows THIS box's room identity, so reading
      # it while bridging a remote room would answer local posts in the wrong
      # room.
      return unless room_source?('posts') && @room_local

      posts = @client.room_posts
      newest = @state['last_post_ts'].to_i
      posts.sort_by { |p| p['t'].to_i }.each do |post|
        ts = post['t'].to_i
        author = post['a'].to_s.downcase        # the endpoint hexes uppercase
        text = post['x'].to_s
        next if ts <= @state['last_post_ts'].to_i

        newest = [newest, ts].max
        # Posts authored by this companion are NOT skipped: a phone app driving
        # this same identity is the operator's own way into the room. Loops are
        # prevented at the other end instead -- outgoing replies can never
        # begin with the trigger (see #send_parts).
        handle_room(text: text, author: author, timestamp: ts)
      end
      if newest > @state['last_post_ts'].to_i
        @state['last_post_ts'] = newest
        @state.save
      end
    end

    def room_source?(kind)
      return false if @room.nil?

      src = (@room_src || @cfg['room']['source']).to_s
      @cfg['room']['enabled'] && (src == 'both' || src == kind)
    end

    def handle_room(text:, author:, timestamp:)
      prompt = match_trigger(text)
      return if prompt.nil?

      # Digest, not String#hash: that is seeded per process, so a restart would
      # miss its own persisted dedup entries and answer twice.
      key = "room:#{timestamp}:#{Digest::SHA256.hexdigest(text)[0, 12]}"
      return if @state.seen?(key)

      @state.mark_seen(key)
      sender = @comp.name_for(author)
      MeshLLM.log "<- room #{sender}: #{text[0, 90]}"
      scope = @cfg['room']['reply'] == 'dm' ? "room-dm:#{author}" : 'room'
      enqueue(prompt: prompt, sender: author.empty? ? sender : author, scope: scope,
              reply: { kind: :room, author: author })
    end

    def handle_dm(msg)
      # A room push arrives as a signed message from the room itself; treat it
      # as a room post, not a DM.
      if @room && msg[:from].start_with?(@room.prefix[0, 8]) && msg[:author]
        return unless room_source?('push')

        handle_room(text: msg[:text], author: msg[:author], timestamp: msg[:timestamp])
        return
      end
      return unless @cfg['dm']['enabled']

      prompt = match_trigger(msg[:text])
      return if prompt.nil?

      key = "dm:#{msg[:from]}:#{msg[:timestamp]}:#{Digest::SHA256.hexdigest(msg[:text])[0, 12]}"
      return if @state.seen?(key)

      @state.mark_seen(key)
      MeshLLM.log "<- dm #{@comp.name_for(msg[:from])}: #{msg[:text][0, 90]}"
      enqueue(prompt: prompt, sender: msg[:from], scope: "dm:#{msg[:from]}",
              reply: { kind: :dm, prefix: msg[:from] })
    end

    # --------------------------------------------------------------- output

    def send_parts(reply, parts)
      gap = @cfg['chunk_gap_seconds'].to_i
      parts.each_with_index do |part, i|
        sleep(gap) if i.positive?               # be kind to the airwaves
        # Our own posts come back to us through the room; a reply that started
        # with the trigger would answer itself forever.
        part = part.sub(/\A#{Regexp.escape(@trigger)}+\s*/, '')
        next if part.empty?

        ok = deliver(reply, part)
        MeshLLM.log "-> #{part}#{ok ? '' : '  [not delivered]'}"
      end
    end

    def deliver(reply, text)
      case reply[:kind]
      when :room
        # Posting into a room = a plain text message to the room server, which
        # stores it and pushes it to every other logged-in client.
        if @room && @cfg['room']['reply'] != 'dm'
          ok = @comp.send_dm(@room.prefix, text)
          return true if ok

          MeshLLM.log 'room: post rejected, re-logging in'
          room_login
          return @comp.send_dm(@room.prefix, text)
        end
        return false if reply[:author].to_s.length < 12  # need 6 bytes to DM

        @comp.send_dm(reply[:author], text)
      when :dm
        @comp.send_dm(reply[:prefix], text)
      else false
      end
    rescue Error => e
      MeshLLM.log "!! send failed: #{e.message}"
      false
    end

    # --------------------------------------------------------------- worker

    def worker
      until @stop
        job = begin
          @queue.pop(true)
        rescue ThreadError
          sleep 0.25
          next
        end
        begin
          answer_job(job)
        rescue StandardError => e
          # One bad job must not take the worker down with it.
          MeshLLM.log "!! worker: #{e.class}: #{e.message}"
        end
      end
    end

    # Airtime is precious and strangers do not want our stack traces.
    def on_air_error(message)
      msg = case message
            when /timed out/ then 'the model took too long, try again'
            when /unreachable|ECONNREFUSED/ then 'the model host is down'
            when /HTTP 5|Compute error/ then 'the model backend is having a moment, try again'
            when /HTTP 4/ then 'the model rejected that request'
            when /no answer/ then 'no answer came back, try rephrasing'
            else 'something went wrong reaching the model'
            end
      Shaper.clip("sorry, #{msg}", @limit)
    end

    def answer_job(job)
      turns = @cfg['llm']['history_turns'].to_i
      soft = [@limit - 15, 40].max
      messages = [{ 'role' => 'system',
                    'content' => (@cfg['llm']['system_prompt'] ||
                                  format(SYSTEM_PROMPT, limit: @limit, soft: soft)) }]
      messages.concat(@state.history(job.scope, turns))
      messages << { 'role' => 'user', 'content' => job.prompt }

      t0 = Time.now
      begin
        answer = @llm.chat(messages)
      rescue Error => e
        # Full detail to the log; the mesh gets a short human sentence rather
        # than a clipped lump of backend JSON.
        MeshLLM.log "!! llm: #{e.message}"
        send_parts(job.reply, [on_air_error(e.message)])
        return
      end
      parts = Shaper.chunk(answer, @limit, @chunks)
      @answered += 1
      @state.remember(job.scope, job.prompt, Shaper.flatten(answer), turns)
      @state.save
      MeshLLM.log format('   %.1fs, %d bytes raw -> %d part(s)',
                         Time.now - t0, Shaper.flatten(answer).bytesize, parts.length)
      send_parts(job.reply, parts.empty? ? ['(no answer)'] : parts)
    end

    # ------------------------------------------------------------ main loop

    def run(replay: false)
      connect_with_retry
      seq, = @comp.poll_archive(@state['arch_seq'].to_i)
      unless replay
        @state['arch_seq'] = seq
        # Don't answer posts that predate this run.
        newest = room_source?('posts') && @room_local ? (@client.room_posts.map { |p| p['t'].to_i }.max || 0) : 0
        @state['last_post_ts'] = [newest, @state['last_post_ts'].to_i].max
        @state.save
        MeshLLM.log "cursors: archive #{seq}, newest post #{@state['last_post_ts']} " \
                    '(--replay to answer the backlog)'
      end

      worker_thread = Thread.new { worker }
      worker_thread.abort_on_exception = false
      poll = [@cfg['poll_seconds'].to_i, 1].max
      next_drain_check = 0.0
      drain_ok = false
      # Nodes advert constantly, so the contact list a sender's name comes from
      # goes stale fast.
      refresh_every = [@cfg['contacts_refresh_minutes'].to_i, 1].max * 60
      next_refresh = Time.now.to_f + refresh_every

      until @stop
        begin
          room_login if @room && @next_login.positive? && Time.now.to_f > @next_login
          if Time.now.to_f > next_refresh
            @comp.load_contacts
            next_refresh = Time.now.to_f + refresh_every
          end

          if Time.now.to_f >= next_drain_check
            drain_ok = !@comp.phone_attached?
            next_drain_check = Time.now.to_f + 30
          end
          @comp.drain.each { |m| handle_dm(m) } if drain_ok

          seq, msgs, = @comp.poll_archive(@state['arch_seq'].to_i)
          if seq != @state['arch_seq'].to_i
            @state['arch_seq'] = seq
            @state.save
          end
          msgs.each { |m| handle_dm(m) }

          poll_room_posts
        rescue Error => e
          MeshLLM.log "!! node: #{e.message}"
          sleep [poll * 3, 30].min
          next
        rescue StandardError => e
          MeshLLM.log "!! loop: #{e.class}: #{e.message}"
        end
        sleep poll
      end

      @queue.close if @queue.respond_to?(:close)
      worker_thread.kill
      @client.close
      MeshLLM.log 'stopped'
    end
  end

  # -------------------------------------------------------------------- CLI

  module CLI
    module_function

    def deep_merge(base, over)
      base.merge(over || {}) do |_k, a, b|
        a.is_a?(Hash) && b.is_a?(Hash) ? deep_merge(a, b) : b
      end
    end

    def load_config(path)
      candidates = path ? [path] : [File.join(__dir__, 'config.json'),
                                    File.expand_path('~/.meshllm/config.json')]
      cfg = JSON.parse(JSON.generate(DEFAULTS))
      found = candidates.find { |c| c && File.exist?(c) }
      if found
        cfg = deep_merge(cfg, JSON.parse(File.read(found)))
        cfg['_path'] = found
      elsif path
        abort "config not found: #{path}"
      end
      cfg['node']['host'] = ENV['MESHLLM_HOST'] if ENV['MESHLLM_HOST']
      cfg['node']['password'] = ENV['MESHLLM_PASSWORD'] if ENV['MESHLLM_PASSWORD']
      cfg
    end

    def probe(cfg)
      llm = Llm.new(cfg['llm'])
      health = llm.health
      puts "llm       : #{cfg['llm']['url']}"
      puts "            status #{health['status']}, backends #{health.dig('backends', 'healthy')}/#{health.dig('backends', 'total')}"
      models = llm.models
      mark = models.include?(cfg['llm']['model']) ? 'ok' : 'NOT FOUND'
      puts "model     : #{cfg['llm']['model']} (#{mark})"
      puts "            available: #{models.join(', ')}" unless models.empty?

      client = PanelClient.new(cfg['node'])
      comp = Companion.new(client).init
      puts "companion : #{comp.self_name} #{comp.self_pubkey[0, 16]}"
      puts "phone app : #{comp.phone_attached? ? 'attached' : 'none'}"
      comp.load_contacts
      local = client.console('identities').to_s[/room=([0-9a-fA-F]+)/, 1].to_s.downcase
      want = cfg['room']['name'].to_s.strip
      puts "rooms     : #{comp.rooms.empty? ? 'none in contacts (run pair-room)' : ''}"
      comp.rooms.each do |r|
        tags = []
        tags << 'this box' if !local.empty? && r.prefix.start_with?(local)
        tags << 'configured' if !want.empty? && r.name.strip.casecmp(want).zero?
        puts "            #{r.prefix[0, 12]} #{r.name}#{tags.empty? ? '' : "  <- #{tags.join(', ')}"}"
      end
      puts "            room.name is unset -- set it to one of the above" if want.empty? && comp.rooms.length > 1
      posts = client.room_posts
      puts "local room: #{posts.length} posts stored, newest #{posts.map { |p| p['t'].to_i }.max || 0}"
      posts.last(3).each { |p| puts "            #{p['a'][0, 8]}: #{p['x'][0, 70]}" }
      puts "contacts  : #{comp.contacts.length}"
      comp.contacts.first(10).each { |c| puts "            #{c.prefix[0, 12]} t#{c.type} #{c.name}" }
      puts "reply cap : #{[cfg['reply_limit'].to_i, Proto::MAX_POST_TEXT].min} bytes"
    end

    # Make the local room advert so the companion learns it as a contact. The
    # shared radio loops transmitted frames back to sibling identities, so the
    # companion hears its own box's room.
    def pair_room(cfg)
      client = PanelClient.new(cfg['node'])
      comp = Companion.new(client).init
      comp.load_contacts
      local = client.console('identities').to_s[/room=([0-9a-fA-F]+)/, 1].to_s.downcase
      abort 'could not read the local room identity from the console' if local.empty?

      mine = ->(list) { list.find { |r| r.prefix.start_with?(local) } }
      if (room = mine.call(comp.rooms))
        puts "this box's room is already a contact: #{room.name} #{room.prefix[0, 12]}"
        return
      end

      # Read the room's own key straight off the device and add the contact
      # directly. Asking the room to advert would work too, but this box's room
      # may be deliberately private (advert.interval 0) and an advert announces
      # its existence and key to everyone in radio range.
      pubkey = client.console('room get public.key').to_s[/[0-9A-Fa-f]{64}/].to_s.downcase
      name = client.console('room get name').to_s.sub(/\A>\s*/, '').strip
      abort "could not read the room's public key from the console" if pubkey.empty?

      puts "adding #{name.inspect} #{pubkey[0, 12]} as a contact (no advert, nothing transmitted)"
      abort 'the companion rejected the contact (table full?)' \
        unless comp.add_contact(pubkey, name, Proto::ADV_TYPE_ROOM)

      comp.load_contacts
      room = comp.rooms.find { |r| r.pubkey == pubkey }
      if room
        puts "paired: #{room.name} #{room.prefix[0, 12]}"
        puts "set room.name to #{room.prefix[0, 8].inspect} (a prefix, since another contact " \
             'may share the name) and room.password to the room password.'
      else
        puts 'contact accepted but not visible in the list yet -- re-run probe in a moment.'
      end
    end

    # Post into the bridged room as the companion. Useful for a live end-to-end
    # check: the room stores it, the bridge reads it back and answers.
    def say(cfg, text)
      client = PanelClient.new(cfg['node'])
      comp = Companion.new(client).init
      comp.load_contacts
      want = cfg['room']['name'].to_s.strip
      room = comp.rooms.find { |r| r.prefix.start_with?(want.downcase) } ||
             comp.rooms.find { |r| r.name.strip.casecmp(want).zero? }
      abort "room #{want.inspect} is not a contact -- run pair-room" if room.nil?

      unless cfg['room']['password'].to_s.empty?
        comp.login_room(room, cfg['room']['password'].to_s)
        sleep 3
      end
      puts(comp.send_dm(room.prefix, text) ? "posted to #{room.name}: #{text}" : 'post rejected by the device')
    end

    def ask(cfg, text)
      limit = [cfg['reply_limit'].to_i, Proto::MAX_POST_TEXT].min
      soft = [limit - 15, 40].max
      llm = Llm.new(cfg['llm'])
      t0 = Time.now
      answer = llm.chat([{ 'role' => 'system',
                           'content' => cfg['llm']['system_prompt'] ||
                                        format(SYSTEM_PROMPT, limit: limit, soft: soft) },
                         { 'role' => 'user', 'content' => text }])
      puts format('(%.1fs, %d bytes raw)', Time.now - t0, Shaper.flatten(answer).bytesize)
      Shaper.chunk(answer, limit, [cfg['max_chunks'].to_i, 1].max).each do |p|
        puts format('  [%3d] %s', p.bytesize, p)
      end
    end

    def shape(cfg, text)
      limit = [cfg['reply_limit'].to_i, Proto::MAX_POST_TEXT].min
      puts "flattened: #{Shaper.flatten(text)}"
      Shaper.chunk(text, limit, [cfg['max_chunks'].to_i, 1].max).each do |p|
        puts format('  [%3d] %s', p.bytesize, p)
      end
    end

    # Exercise the frame codec against synthetic frames built to the layouts in
    # examples/companion_radio/MyMesh.cpp -- these byte offsets are the easiest
    # thing to get wrong and the hardest to spot at runtime.
    def selftest
      fails = []
      check = lambda do |name, got, want|
        if got == want
          puts "  ok   #{name}"
        else
          puts "  FAIL #{name}: got #{got.inspect}, want #{want.inspect}"
          fails << name
        end
      end

      # plain DM, v3: [0]=16 [1]=snr*4 [2,3]=rsvd [4..9]=prefix [10]=path
      # [11]=txt_type [12..15]=ts [16..]=text
      dm = [Proto::RESP_CONTACT_MSG_V3, 40, 0, 0].pack('C4') +
           ['aabbccddeeff'].pack('H*') + [2, Proto::TXT_TYPE_PLAIN].pack('C2') +
           [1_700_000_000].pack('V') + '@hello there'
      m = Proto.parse_contact_msg(dm)
      check.call('dm from', m[:from], 'aabbccddeeff')
      check.call('dm text', m[:text], '@hello there')
      check.call('dm ts', m[:timestamp], 1_700_000_000)
      check.call('dm snr', m[:snr], 10.0)
      check.call('dm author', m[:author], nil)

      # room push: TXT_TYPE_SIGNED_PLAIN, author prefix in the signature slot
      push = [Proto::RESP_CONTACT_MSG_V3, 0xFC, 0, 0].pack('C4') +
             ['112233445566'].pack('H*') +
             [0, Proto::TXT_TYPE_SIGNED_PLAIN].pack('C2') + [42].pack('V') +
             ['deadbeef'].pack('H*') + '@what is SF12'
      p2 = Proto.parse_contact_msg(push)
      check.call('push author', p2[:author], 'deadbeef')
      check.call('push text', p2[:text], '@what is SF12')
      check.call('push snr', p2[:snr], -1.0)

      # legacy (non-v3) contact message
      old = [Proto::RESP_CONTACT_MSG].pack('C') + ['aabbccddeeff'].pack('H*') +
            [0, Proto::TXT_TYPE_PLAIN].pack('C2') + [7].pack('V') + '@hi'
      check.call('legacy text', Proto.parse_contact_msg(old)[:text], '@hi')

      # response body framing and the archive envelope
      frames = Proto.split_frames([3].pack('v') + 'abc' + [2].pack('v') + 'de')
      check.call('split_frames', frames, %w[abc de])
      arch = [9].pack('V') + [8].pack('V') + [2].pack('v') + 'hi' +
             [9].pack('V') + [1].pack('v') + 'x'
      latest, entries = Proto.parse_archive(arch)
      check.call('archive latest', latest, 9)
      check.call('archive entries', entries, [[8, 'hi'], [9, 'x']])

      # command builders
      txt = Proto.send_txt('aabbccddeeff', 'yo', 1_700_000_000)
      check.call('send_txt len', txt.bytesize, 3 + 4 + 6 + 2)
      check.call('send_txt head', txt.byteslice(0, 3).unpack('C3'),
                 [Proto::CMD_SEND_TXT_MSG, Proto::TXT_TYPE_PLAIN, 0])
      check.call('send_txt pubkey', Proto.hex(txt, 7, 6), 'aabbccddeeff')
      login = Proto.send_login('ab' * 32, 'secret')
      check.call('send_login len', login.bytesize, 1 + 32 + 6)

      # shaping stays inside the byte budget and never splits a codepoint
      long = ('naïve mesh radio ' * 20) + '🛰️'
      one = Shaper.chunk(long, 140, 1)
      check.call('shape single count', one.length, 1)
      check.call('shape single fits', one.first.bytesize <= 140, true)
      check.call('shape single valid utf8', one.first.valid_encoding?, true)
      three = Shaper.chunk(long, 140, 3)
      check.call('shape parts fit', three.all? { |p| p.bytesize <= 140 }, true)
      check.call('shape parts numbered', three.first.start_with?('1/3 '), true)
      check.call('shape short passthrough', Shaper.chunk('hi', 140, 1), ['hi'])
      check.call('shape strips markdown',
                 Shaper.chunk("**bold**\n- item\n- two", 140, 1).first,
                 'bold; item; two')

      puts(fails.empty? ? 'selftest: all checks passed' : "selftest: #{fails.length} FAILED")
      exit(fails.empty? ? 0 : 1)
    end

    def main(argv)
      options = { config: nil, verbose: false, replay: false }
      parser = OptionParser.new do |o|
        o.banner = 'Usage: meshllm.rb [run|probe|pair-room|say|ask|shape|console|selftest] [options] [text]'
        o.on('--config PATH', 'config file') { |v| options[:config] = v }
        o.on('--replay', 'also answer messages already in the backlog') { options[:replay] = true }
        o.on('--verbose', 'noisier logging') { options[:verbose] = true }
        o.on('--version') { puts "meshllm #{VERSION}"; exit }
        o.on('-h', '--help') { puts o; exit }
      end
      args = parser.parse(argv)
      command = args.shift || 'run'
      cfg = load_config(options[:config])

      case command
      when 'probe' then probe(cfg)
      when 'pair-room' then pair_room(cfg)
      when 'say'
        abort 'usage: meshllm.rb say "text to post in the room"' if args.empty?

        say(cfg, args.join(' '))
      when 'selftest' then selftest
      when 'console'
        abort 'usage: meshllm.rb console "room get name"' if args.empty?

        puts PanelClient.new(cfg['node']).console(args.join(' ')).to_s.strip
      when 'ask'
        abort 'usage: meshllm.rb ask "your question"' if args.empty?

        ask(cfg, args.join(' '))
      when 'shape' then shape(cfg, args.empty? ? $stdin.read : args.join(' '))
      when 'run'
        bridge = Bridge.new(cfg, verbose: options[:verbose])
        %w[INT TERM].each do |sig|
          Signal.trap(sig) { bridge.stop! }
        end
        bridge.run(replay: options[:replay])
      else
        abort "unknown command #{command}\n#{parser}"
      end
    rescue Error => e
      abort "meshllm: #{e.message}"
    rescue Interrupt
      exit 130
    end
  end
end

MeshLLM::CLI.main(ARGV) if $PROGRAM_NAME == __FILE__
