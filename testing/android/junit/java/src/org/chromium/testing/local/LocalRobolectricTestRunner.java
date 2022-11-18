// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.testing.local;

import org.junit.runners.model.FrameworkMethod;
import org.junit.runners.model.InitializationError;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;
import org.robolectric.internal.bytecode.InstrumentationConfiguration;

/**
 * A custom Robolectric Junit4 Test Runner with minimal Chromium-specific settings. Most test cases
 * should prefer {@link org.chromium.base.test.BaseRobolectricTestRunner} in order to initialize
 * base globals.
 */
public class LocalRobolectricTestRunner extends RobolectricTestRunner {
    public static final int DEFAULT_SDK = 28;
    private static final String DEFAULT_PACKAGE_NAME = "org.robolectric.default";

    static {
        // Setting robolectric.offline which tells Robolectric to look for runtime dependency
        // JARs from a local directory and to not download them from Maven.
        System.setProperty("robolectric.offline", "true");
    }

    public LocalRobolectricTestRunner(Class<?> testClass) throws InitializationError {
        super(testClass);
    }

    @Override
    protected Config buildGlobalConfig() {
        return new Config.Builder()
                .setSdk(DEFAULT_SDK)
                // Shadows to fix robolectric shortcomings.
                .setShadows(new Class[] {CustomShadowApplicationPackageManager.class})
                .build();
    }

    /**
     * This is to avoid a bug in robolectric's shadows-playservices - this workaround is from
     * https://github.com/robolectric/robolectric/issues/7269.
     */
    @Override
    protected InstrumentationConfiguration createClassLoaderConfig(FrameworkMethod method) {
        return new InstrumentationConfiguration.Builder(super.createClassLoaderConfig(method))
                .doNotInstrumentClass("com.google.android.gms.common.api.ApiException")
                .build();
    }
}
