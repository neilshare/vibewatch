import Foundation

public enum AgentCard: String, Codable, CaseIterable, Equatable, Sendable {
    case codex
    case workbuddy
    case antigravity
}

public struct QuotaSnapshot: Codable, Equatable, Sendable {
    public var remainingPercent: Double
    public var resetInSeconds: Int
    public var card: AgentCard?
    public var credits: Double?
    public var totalCredits: Double?

    public init(
        remainingPercent: Double,
        resetInSeconds: Int,
        card: AgentCard? = nil,
        credits: Double? = nil,
        totalCredits: Double? = nil
    ) {
        self.remainingPercent = remainingPercent
        self.resetInSeconds = resetInSeconds
        self.card = card
        self.credits = credits
        self.totalCredits = totalCredits
    }

    enum CodingKeys: String, CodingKey {
        case remainingPercent = "remaining_percent"
        case resetInSeconds = "reset_in_seconds"
        case card
        case credits
        case totalCredits = "total_credits"
    }
}

public struct ApprovalRequestV2: Codable, Equatable, Sendable {
    public let version = 2
    public let kind = "approval_request"
    public let requestID: UUID
    public let card: AgentCard
    public let agentID: Int
    public let operationType: String
    public let summary: String
    public let ttlMs: Int

    public init(requestID: UUID, card: AgentCard, agentID: Int, operationType: String, summary: String, ttlMs: Int) {
        self.requestID = requestID
        self.card = card
        self.agentID = agentID
        self.operationType = operationType
        self.summary = summary
        self.ttlMs = ttlMs
    }

    enum CodingKeys: String, CodingKey {
        case version, kind, card, summary
        case requestID = "request_id"
        case agentID = "agent_id"
        case operationType = "operation_type"
        case ttlMs = "ttl_ms"
    }

    public func encode(to encoder: Encoder) throws {
        var values = encoder.container(keyedBy: CodingKeys.self)
        try values.encode(version, forKey: .version)
        try values.encode(kind, forKey: .kind)
        try values.encode(requestID.uuidString.lowercased(), forKey: .requestID)
        try values.encode(card, forKey: .card)
        try values.encode(agentID, forKey: .agentID)
        try values.encode(operationType, forKey: .operationType)
        try values.encode(summary, forKey: .summary)
        try values.encode(ttlMs, forKey: .ttlMs)
    }

    public init(from decoder: Decoder) throws {
        let values = try decoder.container(keyedBy: CodingKeys.self)
        guard try values.decode(Int.self, forKey: .version) == 2,
              try values.decode(String.self, forKey: .kind) == "approval_request" else {
            throw DecodingError.dataCorruptedError(forKey: .kind, in: values, debugDescription: "expected approval_request version 2")
        }
        let rawID = try values.decode(String.self, forKey: .requestID)
        guard rawID == rawID.lowercased(), let requestID = UUID(uuidString: rawID) else {
            throw DecodingError.dataCorruptedError(forKey: .requestID, in: values, debugDescription: "request_id must be a canonical lowercase UUID")
        }
        self.requestID = requestID
        card = try values.decode(AgentCard.self, forKey: .card)
        agentID = try values.decode(Int.self, forKey: .agentID)
        operationType = try values.decode(String.self, forKey: .operationType)
        summary = try values.decode(String.self, forKey: .summary)
        ttlMs = try values.decode(Int.self, forKey: .ttlMs)
    }
}

public enum ApprovalDecision: String, Codable, CaseIterable, Equatable, Sendable {
    case approve, reject, expired, cancelled
}

public struct ApprovalDecisionV2: Codable, Equatable, Sendable {
    public let version: Int
    public let kind: String
    public let requestID: UUID
    public let decision: ApprovalDecision
    public let decidedAtMs: UInt32

    public init(version: Int = 2, kind: String = "approval_decision", requestID: UUID, decision: ApprovalDecision, decidedAtMs: UInt32) {
        self.version = version
        self.kind = kind
        self.requestID = requestID
        self.decision = decision
        self.decidedAtMs = decidedAtMs
    }

    enum CodingKeys: String, CodingKey {
        case version, kind, decision
        case requestID = "request_id"
        case decidedAtMs = "decided_at_ms"
    }

    public func encode(to encoder: Encoder) throws {
        var values = encoder.container(keyedBy: CodingKeys.self)
        try values.encode(version, forKey: .version)
        try values.encode(kind, forKey: .kind)
        try values.encode(requestID.uuidString.lowercased(), forKey: .requestID)
        try values.encode(decision, forKey: .decision)
        try values.encode(decidedAtMs, forKey: .decidedAtMs)
    }

    public init(from decoder: Decoder) throws {
        let values = try decoder.container(keyedBy: CodingKeys.self)
        version = try values.decode(Int.self, forKey: .version)
        kind = try values.decode(String.self, forKey: .kind)
        let rawID = try values.decode(String.self, forKey: .requestID)
        guard rawID == rawID.lowercased(), let requestID = UUID(uuidString: rawID) else {
            throw DecodingError.dataCorruptedError(forKey: .requestID, in: values, debugDescription: "request_id must be a canonical lowercase UUID")
        }
        self.requestID = requestID
        decision = try values.decode(ApprovalDecision.self, forKey: .decision)
        decidedAtMs = try values.decode(UInt32.self, forKey: .decidedAtMs)
    }
}
