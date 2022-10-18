/*
 * Copyright (C) 2022 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package com.android.server.art;

import static com.android.server.art.DexUseManager.DexLoader;
import static com.android.server.art.DexUseManager.SecondaryDexInfo;

import static com.google.common.truth.Truth.assertThat;

import static org.mockito.Mockito.any;
import static org.mockito.Mockito.anyInt;
import static org.mockito.Mockito.argThat;
import static org.mockito.Mockito.eq;
import static org.mockito.Mockito.lenient;
import static org.mockito.Mockito.mock;
import static org.mockito.Mockito.when;

import android.content.pm.ApplicationInfo;
import android.os.Binder;
import android.os.SystemProperties;
import android.os.UserHandle;

import androidx.test.filters.SmallTest;

import com.android.server.art.testing.StaticMockitoRule;
import com.android.server.art.wrapper.Environment;
import com.android.server.art.wrapper.Process;
import com.android.server.pm.PackageManagerLocal;
import com.android.server.pm.pkg.AndroidPackage;
import com.android.server.pm.pkg.AndroidPackageSplit;
import com.android.server.pm.pkg.PackageState;

import dalvik.system.PathClassLoader;

import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.junit.runners.Parameterized;
import org.junit.runners.Parameterized.Parameter;
import org.junit.runners.Parameterized.Parameters;
import org.mockito.Mock;

import java.io.File;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.function.Consumer;

@SmallTest
@RunWith(Parameterized.class)
public class DexUseManagerTest {
    private static final String LOADING_PKG_NAME = "com.example.loadingpackage";
    private static final String OWNING_PKG_NAME = "com.example.owningpackage";
    private static final String BASE_APK = "/data/app/" + OWNING_PKG_NAME + "/base.apk";
    private static final String SPLIT_APK = "/data/app/" + OWNING_PKG_NAME + "/split_0.apk";

    @Rule
    public StaticMockitoRule mockitoRule = new StaticMockitoRule(
            SystemProperties.class, Constants.class, Process.class);

    @Parameter(0) public String mVolumeUuid;

    private final UserHandle mUserHandle = Binder.getCallingUserHandle();

    @Mock private PackageManagerLocal.FilteredSnapshot mSnapshot;
    private DexUseManager mDexUseManager = DexUseManager.getInstance();
    private String mCeDir;
    private String mDeDir;

    @Parameters(name = "volumeUuid={0}")
    public static Iterable<? extends Object> data() {
        List<String> volumeUuids = new ArrayList<>();
        volumeUuids.add(null);
        volumeUuids.add("volume-abcd");
        return volumeUuids;
    }

    @Before
    public void setUp() throws Exception {
        // No ISA translation.
        lenient()
                .when(SystemProperties.get(argThat(arg -> arg.startsWith("ro.dalvik.vm.isa."))))
                .thenReturn("");

        lenient().when(Constants.getPreferredAbi()).thenReturn("arm64-v8a");
        lenient().when(Constants.getNative64BitAbi()).thenReturn("arm64-v8a");
        lenient().when(Constants.getNative32BitAbi()).thenReturn("armeabi-v7a");

        lenient().when(Process.isIsolated(anyInt())).thenReturn(false);

        PackageState loadingPkgState = createPackageState(LOADING_PKG_NAME, "armeabi-v7a");
        lenient().when(mSnapshot.getPackageState(eq(LOADING_PKG_NAME))).thenReturn(loadingPkgState);
        PackageState owningPkgState = createPackageState(OWNING_PKG_NAME, "arm64-v8a");
        lenient().when(mSnapshot.getPackageState(eq(OWNING_PKG_NAME))).thenReturn(owningPkgState);

        lenient()
                .doAnswer(invocation -> {
                    var consumer = invocation.<Consumer<PackageState>>getArgument(0);
                    consumer.accept(loadingPkgState);
                    consumer.accept(owningPkgState);
                    return null;
                })
                .when(mSnapshot)
                .forAllPackageStates(any());

        mCeDir = Environment
                         .getDataUserCePackageDirectory(mVolumeUuid,
                                 Binder.getCallingUserHandle().getIdentifier(), OWNING_PKG_NAME)
                         .toString();
        mDeDir = Environment
                         .getDataUserDePackageDirectory(mVolumeUuid,
                                 Binder.getCallingUserHandle().getIdentifier(), OWNING_PKG_NAME)
                         .toString();

        mDexUseManager.clear();
    }

    @Test
    public void testPrimaryDexOwned() {
        mDexUseManager.addDexUse(mSnapshot, OWNING_PKG_NAME, Map.of(BASE_APK, "CLC"));

        assertThat(mDexUseManager.getPrimaryDexLoaders(OWNING_PKG_NAME, BASE_APK))
                .containsExactly(DexLoader.create(OWNING_PKG_NAME, false /* isolatedProcess */));
        assertThat(mDexUseManager.isPrimaryDexUsedByOtherApps(OWNING_PKG_NAME, BASE_APK)).isFalse();

        assertThat(mDexUseManager.getPrimaryDexLoaders(OWNING_PKG_NAME, SPLIT_APK)).isEmpty();
        assertThat(mDexUseManager.isPrimaryDexUsedByOtherApps(OWNING_PKG_NAME, SPLIT_APK))
                .isFalse();
    }

    @Test
    public void testPrimaryDexOwnedIsolated() {
        when(Process.isIsolated(anyInt())).thenReturn(true);
        mDexUseManager.addDexUse(mSnapshot, OWNING_PKG_NAME, Map.of(BASE_APK, "CLC"));

        assertThat(mDexUseManager.getPrimaryDexLoaders(OWNING_PKG_NAME, BASE_APK))
                .containsExactly(DexLoader.create(OWNING_PKG_NAME, true /* isolatedProcess */));
        assertThat(mDexUseManager.isPrimaryDexUsedByOtherApps(OWNING_PKG_NAME, BASE_APK)).isTrue();

        assertThat(mDexUseManager.getPrimaryDexLoaders(OWNING_PKG_NAME, SPLIT_APK)).isEmpty();
        assertThat(mDexUseManager.isPrimaryDexUsedByOtherApps(OWNING_PKG_NAME, SPLIT_APK))
                .isFalse();
    }

    @Test
    public void testPrimaryDexOwnedSplitIsolated() {
        when(Process.isIsolated(anyInt())).thenReturn(true);
        mDexUseManager.addDexUse(mSnapshot, OWNING_PKG_NAME, Map.of(SPLIT_APK, "CLC"));

        assertThat(mDexUseManager.getPrimaryDexLoaders(OWNING_PKG_NAME, BASE_APK)).isEmpty();
        assertThat(mDexUseManager.isPrimaryDexUsedByOtherApps(OWNING_PKG_NAME, BASE_APK)).isFalse();

        assertThat(mDexUseManager.getPrimaryDexLoaders(OWNING_PKG_NAME, SPLIT_APK))
                .containsExactly(DexLoader.create(OWNING_PKG_NAME, true /* isolatedProcess */));
        assertThat(mDexUseManager.isPrimaryDexUsedByOtherApps(OWNING_PKG_NAME, SPLIT_APK)).isTrue();
    }

    @Test
    public void testPrimaryDexOthers() {
        mDexUseManager.addDexUse(mSnapshot, LOADING_PKG_NAME, Map.of(BASE_APK, "CLC"));

        assertThat(mDexUseManager.getPrimaryDexLoaders(OWNING_PKG_NAME, BASE_APK))
                .containsExactly(DexLoader.create(LOADING_PKG_NAME, false /* isolatedProcess */));
        assertThat(mDexUseManager.isPrimaryDexUsedByOtherApps(OWNING_PKG_NAME, BASE_APK)).isTrue();

        assertThat(mDexUseManager.getPrimaryDexLoaders(OWNING_PKG_NAME, SPLIT_APK)).isEmpty();
        assertThat(mDexUseManager.isPrimaryDexUsedByOtherApps(OWNING_PKG_NAME, SPLIT_APK))
                .isFalse();
    }

    /** Checks that it ignores and dedups things correctly. */
    @Test
    public void testPrimaryDexMultipleEntries() {
        // These should be ignored.
        mDexUseManager.addDexUse(mSnapshot, "android", Map.of(BASE_APK, "CLC"));
        mDexUseManager.addDexUse(mSnapshot, OWNING_PKG_NAME,
                Map.of("/data/app/" + OWNING_PKG_NAME + "/non-existing.apk", "CLC"));

        // Some of these should be deduped.
        mDexUseManager.addDexUse(
                mSnapshot, OWNING_PKG_NAME, Map.of(BASE_APK, "CLC", SPLIT_APK, "CLC"));
        mDexUseManager.addDexUse(
                mSnapshot, OWNING_PKG_NAME, Map.of(BASE_APK, "CLC", SPLIT_APK, "CLC"));

        mDexUseManager.addDexUse(mSnapshot, LOADING_PKG_NAME, Map.of(BASE_APK, "CLC"));
        mDexUseManager.addDexUse(mSnapshot, LOADING_PKG_NAME, Map.of(BASE_APK, "CLC"));

        when(Process.isIsolated(anyInt())).thenReturn(true);
        mDexUseManager.addDexUse(mSnapshot, OWNING_PKG_NAME, Map.of(BASE_APK, "CLC"));
        mDexUseManager.addDexUse(mSnapshot, OWNING_PKG_NAME, Map.of(BASE_APK, "CLC"));

        assertThat(mDexUseManager.getPrimaryDexLoaders(OWNING_PKG_NAME, BASE_APK))
                .containsExactly(DexLoader.create(OWNING_PKG_NAME, false /* isolatedProcess */),
                        DexLoader.create(OWNING_PKG_NAME, true /* isolatedProcess */),
                        DexLoader.create(LOADING_PKG_NAME, false /* isolatedProcess */));

        assertThat(mDexUseManager.getPrimaryDexLoaders(OWNING_PKG_NAME, SPLIT_APK))
                .containsExactly(DexLoader.create(OWNING_PKG_NAME, false /* isolatedProcess */));
    }

    @Test
    public void testSecondaryDexOwned() {
        mDexUseManager.addDexUse(mSnapshot, OWNING_PKG_NAME, Map.of(mCeDir + "/foo.apk", "CLC"));

        List<SecondaryDexInfo> dexInfoList = mDexUseManager.getSecondaryDexInfo(OWNING_PKG_NAME);
        assertThat(dexInfoList)
                .containsExactly(SecondaryDexInfo.create(mCeDir + "/foo.apk", mUserHandle, "CLC",
                        Set.of("arm64-v8a"),
                        Set.of(DexLoader.create(OWNING_PKG_NAME, false /* isolatedProcess */)),
                        false /* isUsedByOtherApps */));
        assertThat(dexInfoList.get(0).isClassLoaderContextValid()).isTrue();
    }

    @Test
    public void testSecondaryDexOwnedIsolated() {
        when(Process.isIsolated(anyInt())).thenReturn(true);
        mDexUseManager.addDexUse(mSnapshot, OWNING_PKG_NAME, Map.of(mDeDir + "/foo.apk", "CLC"));

        List<SecondaryDexInfo> dexInfoList = mDexUseManager.getSecondaryDexInfo(OWNING_PKG_NAME);
        assertThat(dexInfoList)
                .containsExactly(SecondaryDexInfo.create(mDeDir + "/foo.apk", mUserHandle, "CLC",
                        Set.of("arm64-v8a"),
                        Set.of(DexLoader.create(OWNING_PKG_NAME, true /* isolatedProcess */)),
                        true /* isUsedByOtherApps */));
        assertThat(dexInfoList.get(0).isClassLoaderContextValid()).isTrue();
    }

    @Test
    public void testSecondaryDexOthers() {
        mDexUseManager.addDexUse(mSnapshot, LOADING_PKG_NAME, Map.of(mCeDir + "/foo.apk", "CLC"));

        List<SecondaryDexInfo> dexInfoList = mDexUseManager.getSecondaryDexInfo(OWNING_PKG_NAME);
        assertThat(dexInfoList)
                .containsExactly(SecondaryDexInfo.create(mCeDir + "/foo.apk", mUserHandle, "CLC",
                        Set.of("armeabi-v7a"),
                        Set.of(DexLoader.create(LOADING_PKG_NAME, false /* isolatedProcess */)),
                        true /* isUsedByOtherApps */));
        assertThat(dexInfoList.get(0).isClassLoaderContextValid()).isTrue();
    }

    @Test
    public void testSecondaryDexUnsupportedClc() {
        mDexUseManager.addDexUse(mSnapshot, LOADING_PKG_NAME,
                Map.of(mCeDir + "/foo.apk", SecondaryDexInfo.UNSUPPORTED_CLASS_LOADER_CONTEXT));

        List<SecondaryDexInfo> dexInfoList = mDexUseManager.getSecondaryDexInfo(OWNING_PKG_NAME);
        assertThat(dexInfoList)
                .containsExactly(SecondaryDexInfo.create(mCeDir + "/foo.apk", mUserHandle,
                        SecondaryDexInfo.UNSUPPORTED_CLASS_LOADER_CONTEXT, Set.of("armeabi-v7a"),
                        Set.of(DexLoader.create(LOADING_PKG_NAME, false /* isolatedProcess */)),
                        true /* isUsedByOtherApps */));
        assertThat(dexInfoList.get(0).isClassLoaderContextValid()).isFalse();
    }

    @Test
    public void testSecondaryDexVariableClc() {
        mDexUseManager.addDexUse(mSnapshot, OWNING_PKG_NAME, Map.of(mCeDir + "/foo.apk", "CLC"));
        mDexUseManager.addDexUse(mSnapshot, LOADING_PKG_NAME, Map.of(mCeDir + "/foo.apk", "CLC2"));

        List<SecondaryDexInfo> dexInfoList = mDexUseManager.getSecondaryDexInfo(OWNING_PKG_NAME);
        assertThat(dexInfoList)
                .containsExactly(SecondaryDexInfo.create(mCeDir + "/foo.apk", mUserHandle,
                        SecondaryDexInfo.VARYING_CLASS_LOADER_CONTEXTS,
                        Set.of("arm64-v8a", "armeabi-v7a"),
                        Set.of(DexLoader.create(OWNING_PKG_NAME, false /* isolatedProcess */),
                                DexLoader.create(LOADING_PKG_NAME, false /* isolatedProcess */)),
                        true /* isUsedByOtherApps */));
        assertThat(dexInfoList.get(0).isClassLoaderContextValid()).isFalse();
    }

    /** Checks that it ignores and dedups things correctly. */
    @Test
    public void testSecondaryDexMultipleEntries() {
        // These should be ignored.
        mDexUseManager.addDexUse(mSnapshot, "android", Map.of(mCeDir + "/foo.apk", "CLC"));
        mDexUseManager.addDexUse(
                mSnapshot, OWNING_PKG_NAME, Map.of("/some/non-existing.apk", "CLC"));

        // Some of these should be deduped.
        mDexUseManager.addDexUse(mSnapshot, OWNING_PKG_NAME,
                Map.of(mCeDir + "/foo.apk", "CLC", mCeDir + "/bar.apk", "CLC"));
        mDexUseManager.addDexUse(mSnapshot, OWNING_PKG_NAME,
                Map.of(mCeDir + "/foo.apk", "UpdatedCLC", mCeDir + "/bar.apk", "UpdatedCLC"));

        mDexUseManager.addDexUse(mSnapshot, LOADING_PKG_NAME, Map.of(mCeDir + "/foo.apk", "CLC"));
        mDexUseManager.addDexUse(
                mSnapshot, LOADING_PKG_NAME, Map.of(mCeDir + "/foo.apk", "UpdatedCLC"));

        mDexUseManager.addDexUse(
                mSnapshot, LOADING_PKG_NAME, Map.of(mCeDir + "/bar.apk", "DifferentCLC"));
        mDexUseManager.addDexUse(
                mSnapshot, LOADING_PKG_NAME, Map.of(mCeDir + "/bar.apk", "UpdatedDifferentCLC"));

        mDexUseManager.addDexUse(mSnapshot, OWNING_PKG_NAME,
                Map.of(mCeDir + "/baz.apk", SecondaryDexInfo.UNSUPPORTED_CLASS_LOADER_CONTEXT));

        when(Process.isIsolated(anyInt())).thenReturn(true);
        mDexUseManager.addDexUse(mSnapshot, OWNING_PKG_NAME, Map.of(mCeDir + "/foo.apk", "CLC"));
        mDexUseManager.addDexUse(mSnapshot, OWNING_PKG_NAME,
                Map.of(mCeDir + "/foo.apk", SecondaryDexInfo.UNSUPPORTED_CLASS_LOADER_CONTEXT));

        List<SecondaryDexInfo> dexInfoList = mDexUseManager.getSecondaryDexInfo(OWNING_PKG_NAME);
        assertThat(dexInfoList)
                .containsExactly(SecondaryDexInfo.create(mCeDir + "/foo.apk", mUserHandle,
                                         "UpdatedCLC", Set.of("arm64-v8a", "armeabi-v7a"),
                                         Set.of(DexLoader.create(OWNING_PKG_NAME,
                                                        false /* isolatedProcess */),
                                                 DexLoader.create(OWNING_PKG_NAME,
                                                         true /* isolatedProcess */),
                                                 DexLoader.create(LOADING_PKG_NAME,
                                                         false /* isolatedProcess */)),
                                         true /* isUsedByOtherApps */),
                        SecondaryDexInfo.create(mCeDir + "/bar.apk", mUserHandle,
                                SecondaryDexInfo.VARYING_CLASS_LOADER_CONTEXTS,
                                Set.of("arm64-v8a", "armeabi-v7a"),
                                Set.of(DexLoader.create(
                                               OWNING_PKG_NAME, false /* isolatedProcess */),
                                        DexLoader.create(
                                                LOADING_PKG_NAME, false /* isolatedProcess */)),
                                true /* isUsedByOtherApps */),
                        SecondaryDexInfo.create(mCeDir + "/baz.apk", mUserHandle,
                                SecondaryDexInfo.UNSUPPORTED_CLASS_LOADER_CONTEXT,
                                Set.of("arm64-v8a"),
                                Set.of(DexLoader.create(
                                        OWNING_PKG_NAME, false /* isolatedProcess */)),
                                false /* isUsedByOtherApps */));
    }

    @Test(expected = IllegalArgumentException.class)
    public void testUnknownPackage() {
        mDexUseManager.addDexUse(mSnapshot, "bogus", Map.of(BASE_APK, "CLC"));
    }

    @Test(expected = IllegalArgumentException.class)
    public void testEmptyMap() {
        mDexUseManager.addDexUse(mSnapshot, OWNING_PKG_NAME, Map.of());
    }

    @Test(expected = IllegalArgumentException.class)
    public void testNullKey() {
        var map = new HashMap<String, String>();
        map.put(null, "CLC");
        mDexUseManager.addDexUse(mSnapshot, OWNING_PKG_NAME, map);
    }

    @Test(expected = IllegalArgumentException.class)
    public void testNonAbsoluteKey() {
        mDexUseManager.addDexUse(mSnapshot, OWNING_PKG_NAME, Map.of("a/b.jar", "CLC"));
    }

    @Test(expected = IllegalArgumentException.class)
    public void testNullValue() {
        var map = new HashMap<String, String>();
        map.put(mCeDir + "/foo.apk", null);
        mDexUseManager.addDexUse(mSnapshot, OWNING_PKG_NAME, map);
    }

    private AndroidPackage createPackage(String packageName) {
        AndroidPackage pkg = mock(AndroidPackage.class);

        var baseSplit = mock(AndroidPackageSplit.class);
        lenient().when(baseSplit.getPath()).thenReturn("/data/app/" + packageName + "/base.apk");
        lenient().when(baseSplit.isHasCode()).thenReturn(true);
        lenient().when(baseSplit.getClassLoaderName()).thenReturn(PathClassLoader.class.getName());

        var split0 = mock(AndroidPackageSplit.class);
        lenient().when(split0.getName()).thenReturn("split_0");
        lenient().when(split0.getPath()).thenReturn("/data/app/" + packageName + "/split_0.apk");
        lenient().when(split0.isHasCode()).thenReturn(true);

        lenient().when(pkg.getSplits()).thenReturn(List.of(baseSplit, split0));

        return pkg;
    }

    private PackageState createPackageState(String packageName, String primaryAbi) {
        PackageState pkgState = mock(PackageState.class);
        lenient().when(pkgState.getPackageName()).thenReturn(packageName);
        AndroidPackage pkg = createPackage(packageName);
        lenient().when(pkgState.getAndroidPackage()).thenReturn(pkg);
        lenient().when(pkgState.getPrimaryCpuAbi()).thenReturn(primaryAbi);
        lenient().when(pkgState.getVolumeUuid()).thenReturn(mVolumeUuid);
        return pkgState;
    }
}
