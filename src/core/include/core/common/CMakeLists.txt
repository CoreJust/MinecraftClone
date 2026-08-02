target_sources(mc
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
        FILES
            Assert.hpp
            Assume.hpp
            AtAppExit.hpp
            ByteReader.hpp
            ByteSerializable.hpp
            ByteWriter.hpp
            CodeExecutionHelper.hpp
            Color.hpp
            ConstString.hpp
            ControlFlow.hpp
            CrashHandler.hpp
            Defer.hpp
            EnumBits.hpp
            HashCombine.hpp
            InputSpan.hpp
            NonCopyable.hpp
            NonMovable.hpp
            SpanUtils.hpp
            StaticInitializer.hpp
            TrivialPair.hpp
            VectorUtils.hpp
            Version.hpp
)
