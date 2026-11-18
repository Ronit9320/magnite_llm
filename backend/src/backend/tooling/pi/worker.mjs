import { randomUUID } from "node:crypto"
import { createInterface } from "node:readline"
import { Agent } from "@earendil-works/pi-agent-core"
import { createAssistantMessageEventStream } from "@earendil-works/pi-ai"

const input = createInterface({ input: process.stdin, crlfDelay: Infinity })
const lines = input[Symbol.asyncIterator]()
const send = (frame) => process.stdout.write(JSON.stringify(frame) + "\n")
const read = async () => {
  const { value, done } = await lines.next()
  if (done) throw new Error("Magnite closed the Pi bridge")
  return JSON.parse(value)
}
const exchange = async (frame, type) => {
  send(frame)
  const result = await read()
  if (result.type !== type) throw new Error("Invalid Magnite reply")
  return result
}

const model = {
  id: "magnite-local", name: "Magnite local model", api: "openai-completions",
  provider: "magnite", baseUrl: "", input: ["text"], reasoning: false,
  contextWindow: 8192, maxTokens: 512,
  cost: { input: 0, output: 0, cacheRead: 0, cacheWrite: 0 },
}
const usage = {
  input: 0, output: 0, cacheRead: 0, cacheWrite: 0, reasoning: 0, totalTokens: 0,
  cost: { input: 0, output: 0, cacheRead: 0, cacheWrite: 0, total: 0 },
}

function assistant(content, stopReason) {
  return { role: "assistant", content, stopReason, usage, api: model.api,
    provider: model.provider, model: model.id, timestamp: Date.now() }
}

const instructions = [
  "You are a local assistant. Answer directly unless the user asks you to take an action.",
  'Reply with exactly one JSON object, using one of these forms:',
  '{"answer":"your final answer"}',
  '{"action":"open","target":"an absolute file path, folder path, or HTTP(S) URL"}',
  '{"action":"command","executable":"program name or path","arguments":["one","argument"]}',
  "Actions are proposals. Magnite will ask the user for an exact approval.",
  "Report the actual tool receipt. If an action was denied, do not propose it again.",
].join("\n")

const tools = [
  {
    name: "open", label: "Open target", description: "Request approval to open a file, folder or URL.",
    parameters: { type: "object", required: ["target"], additionalProperties: false,
      properties: { target: { type: "string", minLength: 1, maxLength: 4096 } } },
  },
  {
    name: "command", label: "Run command", description: "Request approval to launch a command with an argument list.",
    parameters: { type: "object", required: ["executable", "arguments"], additionalProperties: false,
      properties: {
        executable: { type: "string", minLength: 1, maxLength: 4096 },
        arguments: { type: "array", maxItems: 32, items: { type: "string", maxLength: 4096 } },
      } },
  },
].map(tool => ({
  ...tool,
  async execute(_id, args) {
    const receipt = await exchange({ type: "proposal", proposal: { ...args, action: tool.name } }, "receipt")
    return { content: [{ type: "text", text: JSON.stringify(receipt.result) }], details: {} }
  },
}))

function flatten(message) {
  if (typeof message.content === "string") return message.content
  return message.content.map(block => {
    if (block.type === "text") return block.text
    if (block.type === "toolCall") return JSON.stringify({ ...block.arguments, action: block.name })
    return ""
  }).join("\n")
}

function asTranscript(messages, allowActions) {
  const system = messages.filter(message => message.role === "system").map(flatten).filter(Boolean)
  const turns = messages.filter(message => message.role !== "system").map(message => {
    if (message.role === "toolResult")
      return { role: "user", content: "Magnite tool receipt: " + flatten(message) }
    return { role: message.role, content: flatten(message) }
  })
  return [{ role: "system", content: [
    instructions, ...system,
    allowActions ? "" : 'Action limit reached. Reply using only {"answer":"your final answer"}.',
  ].filter(Boolean).join("\n\n") }, ...turns]
}

function response(raw, allowActions) {
  const text = raw.trim()
  let value
  try { value = JSON.parse(text) } catch { return assistant([{ type: "text", text }], "stop") }
  if (typeof value?.answer === "string" && Object.keys(value).length === 1)
    return assistant([{ type: "text", text: value.answer }], "stop")
  const tool = tools.find(tool => tool.name === value?.action)
  if (!tool) return assistant([{ type: "text", text }], "stop")
  if (!allowActions) throw new Error("Pi reached the action limit")
  const { action, ...args } = value
  return assistant([{ type: "toolCall", id: randomUUID(), name: tool.name, arguments: args }], "toolUse")
}

try {
  const start = await read()
  if (start.type !== "start") throw new Error("Invalid Pi start frame")
  const actions = start.actions !== false
  let turns = 0
  const agent = new Agent({
    initialState: { model, tools: actions ? tools : [] },
    toolExecution: "sequential",
    async streamFn(_model, context) {
      turns += 1
      if (turns > 5) throw new Error("Pi reached the turn limit")
      const allowed = turns < 5
      const messages = actions ? asTranscript(context.messages, allowed) : context.messages.map(message => ({
        role: message.role, content: flatten(message),
      }))
      if (!actions && start.instructions)
        messages.unshift({ role: "system", content: start.instructions })
      const reply = await exchange({ type: "generate", messages }, "generation")
      const output = actions ? response(reply.text, allowed) : assistant([{ type: "text", text: reply.text }], "stop")
      const stream = createAssistantMessageEventStream()
      stream.push({ type: "start", partial: { ...output, content: [] } })
      stream.push({ type: "done", reason: output.stopReason, message: output })
      stream.end()
      return stream
    },
  })
  const messages = start.messages.map(message => message.role === "assistant"
    ? assistant([{ type: "text", text: message.content }], "stop")
    : { ...message, timestamp: Date.now() })
  await agent.prompt(messages)
  if (agent.state.errorMessage) throw new Error(agent.state.errorMessage)
  const final = agent.state.messages.at(-1)
  if (final?.role !== "assistant" || final.stopReason !== "stop") throw new Error("Pi did not return an answer")
  send({ type: "result", text: flatten(final) })
} catch (error) {
  send({ type: "error", message: String(error.message).slice(0, 500) })
  process.exitCode = 1
} finally {
  input.close()
  process.stdin.destroy()
}
