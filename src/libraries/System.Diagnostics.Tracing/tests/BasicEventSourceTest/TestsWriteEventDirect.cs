// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System;
using System.Collections.Generic;
using System.Diagnostics.Tracing;
using System.Linq;
using Xunit;

namespace BasicEventSourceTests
{
    public class TestsWriteEventDirect
    {
        [Fact]
        public void Test_DefineEvent_Basic()
        {
            using var eventSource = new DirectEventSource();
            Assert.NotNull(eventSource);
            Assert.Equal("DirectEventSource", eventSource.Name);
        }

        [Fact]
        public void Test_WriteEventDirect_ReceivesEvent()
        {
            using var eventSource = new DirectEventSource();
            using var listener = new DirectTestListener();

            listener.EnableEvents(eventSource, EventLevel.Informational);

            var receivedEvents = new List<EventWrittenEventArgs>();
            listener.OnEvent += args => receivedEvents.Add(args);

            eventSource.TestEvent(42);

            // Filter out internal EventSource diagnostic messages (EventId 0)
            var userEvents = receivedEvents.Where(e => e.EventId > 0).ToList();
            Assert.Single(userEvents);
            Assert.Equal(1, userEvents[0].EventId);
            Assert.Equal("TestEvent", userEvents[0].EventName);
        }

        [Fact]
        public void Test_WriteEventDirect_MultipleEvents()
        {
            using var eventSource = new DirectEventSource();
            using var listener = new DirectTestListener();

            listener.EnableEvents(eventSource, EventLevel.Verbose);

            var receivedEvents = new List<EventWrittenEventArgs>();
            listener.OnEvent += args => receivedEvents.Add(args);

            eventSource.TestEvent(1);
            eventSource.VerboseEvent("hello");

            // Filter out internal EventSource diagnostic messages (EventId 0)
            var userEvents = receivedEvents.Where(e => e.EventId > 0).ToList();
            Assert.Equal(2, userEvents.Count);
            Assert.Equal(1, userEvents[0].EventId);
            Assert.Equal(2, userEvents[1].EventId);
        }

        [Fact]
        public void Test_WriteEventDirect_NotEnabled_NoEvent()
        {
            using var eventSource = new DirectEventSource();
            using var listener = new DirectTestListener();

            var receivedEvents = new List<EventWrittenEventArgs>();
            listener.OnEvent += args => receivedEvents.Add(args);

            eventSource.TestEvent(42);

            // Filter out internal EventSource diagnostic messages (EventId 0)
            var userEvents = receivedEvents.Where(e => e.EventId > 0).ToList();
            Assert.Empty(userEvents);
        }

        private sealed class DirectTestListener : EventListener
        {
            public event Action<EventWrittenEventArgs>? OnEvent;

            protected override void OnEventWritten(EventWrittenEventArgs eventData)
            {
                OnEvent?.Invoke(eventData);
            }
        }

        [EventSource(Name = "DirectEventSource")]
        private sealed class DirectEventSource : EventSource
        {
            private readonly nint _testEventHandle;
            private readonly nint _verboseEventHandle;

            public DirectEventSource()
                : base("DirectEventSource", EventSourceSettings.EtwManifestEventFormat)
            {
                _testEventHandle = DefineEvent(
                    eventId: 1,
                    eventName: "TestEvent",
                    level: EventLevel.Informational,
                    keywords: EventKeywords.None,
                    message: "Test event with value {0}");

                _verboseEventHandle = DefineEvent(
                    eventId: 2,
                    eventName: "VerboseEvent",
                    level: EventLevel.Verbose,
                    keywords: EventKeywords.None,
                    message: "Verbose event: {0}");
            }

            public unsafe void TestEvent(int value)
            {
                if (!IsEnabled(EventLevel.Informational, EventKeywords.None))
                    return;

                EventData* data = stackalloc EventData[1];
                data[0].DataPointer = (nint)(&value);
                data[0].Size = sizeof(int);

                WriteEventDirect(1, _testEventHandle, null, null, 1, data);
            }

            public unsafe void VerboseEvent(string message)
            {
                if (!IsEnabled(EventLevel.Verbose, EventKeywords.None))
                    return;

                message ??= string.Empty;
                fixed (char* pMessage = message)
                {
                    EventData* data = stackalloc EventData[1];
                    data[0].DataPointer = (nint)pMessage;
                    data[0].Size = (message.Length + 1) * sizeof(char);

                    WriteEventDirect(2, _verboseEventHandle, null, null, 1, data);
                }
            }
        }
    }

    public class TestsWriteSelfDescribing
    {
        [Fact]
        public void Test_WriteSelfDescribingEvent_Basic()
        {
            using var eventSource = new SelfDescribingEventSource();
            Assert.NotNull(eventSource);
        }

        [Fact]
        public void Test_WriteSelfDescribingEvent_ReceivesEvent()
        {
            using var eventSource = new SelfDescribingEventSource();
            using var listener = new SelfDescribingTestListener();

            listener.EnableEvents(eventSource, EventLevel.Informational);

            var receivedEvents = new List<EventWrittenEventArgs>();
            listener.OnEvent += args => receivedEvents.Add(args);

            eventSource.LogMessage("Hello, World!");

            // Filter to find our specific event by name
            var logEvents = receivedEvents.Where(e => e.EventName == "LogMessage").ToList();
            Assert.Single(logEvents);
            Assert.Equal(EventLevel.Informational, logEvents[0].Level);
        }

        [Fact]
        public void Test_WriteSelfDescribingEvent_NotEnabled_NoEvent()
        {
            using var eventSource = new SelfDescribingEventSource();
            using var listener = new SelfDescribingTestListener();

            var receivedEvents = new List<EventWrittenEventArgs>();
            listener.OnEvent += args => receivedEvents.Add(args);

            eventSource.LogMessage("Should not be received");

            // Filter to find our specific event by name
            var logEvents = receivedEvents.Where(e => e.EventName == "LogMessage").ToList();
            Assert.Empty(logEvents);
        }

        private sealed class SelfDescribingTestListener : EventListener
        {
            public event Action<EventWrittenEventArgs>? OnEvent;

            protected override void OnEventWritten(EventWrittenEventArgs eventData)
            {
                OnEvent?.Invoke(eventData);
            }
        }

        private sealed class SelfDescribingEventSource : EventSource
        {
            private static readonly byte[] s_logMessageNameMetadata = CreateNameMetadata("LogMessage");
            private static readonly byte[] s_logMessageTypeMetadata = Array.Empty<byte>();

            public SelfDescribingEventSource()
                : base("SelfDescribingEventSource", EventSourceSettings.EtwSelfDescribingEventFormat)
            {
            }

            public unsafe void LogMessage(string message)
            {
                if (!IsEnabled(EventLevel.Informational, EventKeywords.None))
                    return;

                EventData* data = stackalloc EventData[4];

                message ??= string.Empty;
                fixed (char* pMessage = message)
                fixed (byte* pNameMetadata = s_logMessageNameMetadata)
                fixed (byte* pTypeMetadata = s_logMessageTypeMetadata)
                {
                    data[3].DataPointer = (nint)pMessage;
                    data[3].Size = (message.Length + 1) * sizeof(char);

                    WriteSelfDescribingEvent(
                        "LogMessage",
                        EventLevel.Informational,
                        EventKeywords.None,
                        EventOpcode.Info,
                        EventTags.None,
                        pNameMetadata,
                        s_logMessageNameMetadata.Length,
                        pTypeMetadata,
                        s_logMessageTypeMetadata.Length,
                        null,
                        null,
                        4,
                        data);
                }
            }

            private static byte[] CreateNameMetadata(string name)
            {
                int utf8Length = System.Text.Encoding.UTF8.GetByteCount(name);
                int totalLength = 2 + utf8Length + 1;
                var metadata = new byte[totalLength];
                ushort size = (ushort)totalLength;
                metadata[0] = (byte)size;
                metadata[1] = (byte)(size >> 8);
                System.Text.Encoding.UTF8.GetBytes(name, 0, name.Length, metadata, 2);
                return metadata;
            }
        }
    }
}
