// Pi agent loop over stdio. Magnite renders and parses every model turn and owns
// tool execution; this worker sequences turns and tool calls with Pi's Agent.
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
  contextWindow: 8192, maxTokens: 768,
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

const text = (message) => typeof message.content === "string" ? message.content
  : message.content.filter(block => block.type === "text").map(block => block.text).join("\n")

// Pi context → OpenAI/Hugging Face chat messages with tool calls and tool results.
function toChat(system, messages) {
  const chat = system ? [{ role: "system", content: system }] : []
  for (const message of messages) {
    if (message.role === "toolResult") {
      chat.push({ role: "tool", tool_call_id: message.toolCallId, name: message.toolName, content: text(message) })
    } else if (message.role === "assistant" && Array.isArray(message.content)
               && message.content.some(block => block.type === "toolCall")) {
      chat.push({ role: "assistant", content: text(message), tool_calls: message.content
        .filter(block => block.type === "toolCall")
        .map(block => ({ id: block.id, type: "function", function: { name: block.name, arguments: block.arguments } })) })
    } else {
      chat.push({ role: message.role, content: text(message) })
    }
  }
  return chat
}

try {
  const start = await read()
  if (start.type !== "start") throw new Error("Invalid Pi start frame")
  const declared = Array.isArray(start.tools) ? start.tools : []
  const maxTurns = Number.isInteger(start.maxTurns) ? start.maxTurns : 6
  const system = start.messages.filter(message => message.role === "system").map(message => message.content).join("\n\n")
  const tools = declared.map(({ function: spec }) => ({
    name: spec.name, label: spec.name, description: spec.description, parameters: spec.parameters,
    async execute(_id, args) {
      const receipt = await exchange({ type: "proposal", proposal: { name: spec.name, arguments: args } }, "receipt")
      return { content: [{ type: "text", text: receipt.text }], details: {}, isError: Boolean(receipt.error) }
    },
  }))
  let turns = 0
  const agent = new Agent({
    initialState: { model, tools },
    toolExecution: "sequential",
    async streamFn(_model, context) {
      turns += 1
      if (turns > maxTurns) throw new Error("Pi reached the turn limit")
      // On the last turn, withhold tools so the model has to answer.
      const offered = turns < maxTurns ? declared : []
      const reply = await exchange({ type: "generate", messages: toChat(system, context.messages), tools: offered },
                                   "generation")
      const calls = Array.isArray(reply.calls) ? reply.calls : []
      const content = []
      if (reply.text) content.push({ type: "text", text: reply.text })
      for (const call of calls) content.push({ type: "toolCall", id: randomUUID(), name: call.name, arguments: call.arguments })
      const output = assistant(content, calls.length ? "toolUse" : "stop")
      const stream = createAssistantMessageEventStream()
      stream.push({ type: "start", partial: { ...output, content: [] } })
      stream.push({ type: "done", reason: output.stopReason, message: output })
      stream.end()
      return stream
    },
  })
  const messages = start.messages.filter(message => message.role !== "system").map(message =>
    message.role === "assistant" ? assistant([{ type: "text", text: message.content }], "stop")
      : { ...message, timestamp: Date.now() })
  await agent.prompt(messages)
  if (agent.state.errorMessage) throw new Error(agent.state.errorMessage)
  const final = agent.state.messages.at(-1)
  if (final?.role !== "assistant" || final.stopReason !== "stop") throw new Error("Pi did not return an answer")
  send({ type: "result", text: text(final) })
} catch (error) {
  send({ type: "error", message: String(error?.message ?? error).slice(0, 500) })
  process.exitCode = 1
} finally {
  input.close()
  process.stdin.destroy()
}
